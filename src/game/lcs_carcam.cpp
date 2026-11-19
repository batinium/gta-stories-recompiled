// [lcs] See lcs_carcam.h. The logic is game/vcs_carcam.cpp's (game-internals.md 12b), kept as it is there; the
// addresses are LCS's, found by aligning VCS's follow-vehicle camera FUN_0029FAF0 with LCS's 0x283EB8 (CCam::Process
// 0x27A380's jump table: modes 18 and 22 both call it), instruction sequences 87% alike (internal notes: LCS features).
#include "game/lcs_carcam.h"

#include "game/game_addrs.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace lcs::carcam
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;

        struct Addrs
        {
            uint32_t followCar;   // the camera behind a vehicle, a0 the CCam
            uint32_t activeCam, cams, camSize; // TheCamera: ActiveCam (u8), Cams[0], sizeof(CCam) (offsets)
            uint32_t lookBehind;  // TheCamera + this: set while looking behind (the heading is taken from Front)
            // CCam offsets
            uint32_t camMode, camTargetEntity, camControl, camAlpha, camBeta, camPitchSpeed, camHeadingSpeed, camTarget;
            uint32_t camPoints[2];
            uint32_t lastAlpha, lastBeta; // last update's alpha / beta (a smaller change is snapped back to these)
            uint32_t pullCap;     // the tuning per vehicle class (rows of 0x3C): +0x2C the cap of its turn toward
            int classes;          // where the vehicle drives, a step
            // entity / vehicle offsets
            uint32_t entityPos, entityForward, moveSpeed, vehicleType, boatType;
            uint32_t cannons;     // CWaterCannons (3 x 0x250, +0 the vehicle spraying); 0 = not known
        };

        constexpr Addrs kLcs{
            0x00283EB8u, // CCam::Process 0x27A380's cases 18 and 22 (jump table 0x3BBBF0) call it as VCS's call 0x29FAF0
            0x10Cu, 0x220u, 0x2B0u, // as lcs_camera.cpp (80 lbu +0x10C, * 0x2B0 + 0x220)
            0x90u,       // VCS TheCamera+0x87C (0x6F4D4C) -> 0x43BC80 at 3 aligned places in the follow-car camera
            0x40u,       // CCam::Process switches on lhu +0x40
            0x274u,      // aligned with VCS's lw +0x70 (the vehicle; then its +0x344 type, +0x760 / +0x638 fields)
            0x5Cu,       // the right stick's turn counts only when lw +0x5C == 3 (0x285128; VCS lw +0x74 == 3)
            0xE0u, 0xF0u, // alpha / beta: 10 + 4, 16 + 7 aligned accesses (VCS +0x78 / +0x7C)
            0xE4u, 0xF4u, // their speeds: 3 + 3 + 3 / 2 + 1 aligned (VCS +0x130 / +0x140)
            0x160u,      // the point looked at: the one store aligned with VCS's +0x1A0
            {0x220u, 0x230u}, // unchanged (2 + 2 aligned stores)
            0x003D6ED8u, 0x003D6EDCu, // VCS 0x487E88 / 0x487E8C, 2 aligned each
            0x00393788u + 0x2Cu, // the rows: VCS 0x487BE8 -> 0x393788 (5 aligned refs, index * 0x3C as VCS); +0x2C
            8,           // classes 0..7 (VCS 0..8): the class picked from the model / type before the row is read
            0x30u, 0x10u,
            0x70u,       // the move speed: VCS lwc1 +0x140 -> +0x70 (5 aligned)
            0x344u,      // the vehicle type: VCS lw +0x354 -> +0x344 (5 aligned); the bike is 5 here (VCS 6)
            1u,          // a boat: GTA III's enum (the ctors store 0..6 to +0x344; low confidence, not seen in a run)
            0x0060CFB0u, // CWaterCannons::Update 0x2CE178 (called from CGame::Process 0x2FBAF0) is VCS's 0x1878F0
                         // instruction for instruction: 3 x 0x250 from here, +0 tested (VCS 0x4E3A60)
        };
        constexpr int kCannonCount = 3;
        constexpr uint32_t kCannonSize = 0x250u;

        constexpr float kTwoPi = 6.2831855f;
        constexpr float kReturnTime = 0.6f;  // the way back eases out: 1 - e^(-t / this) of what is left
        constexpr float kReturnRate = 2.0f;  // ... at most this many radians a second
        constexpr float kReturnLimit = 4.0f; // seconds; the game's own camera has it after that in any case

        // The Rhino's turret (CAutomobile::TankControl 0x13A018, from CAutomobile::ProcessControl 0x128408 at 0x12A358,
        // for the player's tank only: FindPlayerVehicle 0x107278): +0x74C, radians 0..2 pi, turned by the pad's car-gun
        // left / right (0x170EB0(pad, 1): the right stick) times -0.00015 a time step (about 1 rad/s at full tilt); the
        // shell goes along (-sin a, cos a, 0) in the tank's frame (cos 0x3613B8, sin 0x3615A8, then the matrix 0x2D7FB0)
        // on the fire button (0x1716E0), at most every 800 ms. Its camera (mode 18) is the one the mouse turns here.
        constexpr uint32_t kTankControl = 0x0013A018u;
        constexpr uint32_t kTurret = 0x74Cu;
        constexpr float kTurretRate = 2.5f; // rad/s the mouse's turret turns at most (the stick's ~1)
        PS2Runtime::RecompiledFunction g_origTank = nullptr;
        bool g_tankHold = false;      // the player's tank, its turret on the camera: the camera stays where it is put
        uint32_t g_tankFrame = 0;     // the frame TankControl last aimed it
        std::atomic<float> g_turretShown{0.0f};

        uint8_t *g_rdram = nullptr;
        PS2Runtime::RecompiledFunction g_orig = nullptr;
        std::atomic<bool> g_installed{false};

        enum class State
        {
            Idle,   // the game's camera
            Hold,   // the mouse moved: the camera stays where it is put
            Return, // easing back behind the vehicle
        };
        State g_state = State::Idle;
        std::atomic<int> g_stateShown{0};
        float g_turn = 0.0f, g_tilt = 0.0f;
        bool g_stick = false;
        uint32_t g_vehicle = 0, g_frame = 0;
        float g_idle = 0.0f, g_returning = 0.0f;
        float g_pitch = 0.0f;
        float g_lastPos[3] = {};
        bool g_pullOff = false;
        float g_pullCaps[16] = {};

        uint32_t rd32(uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        float rdf(uint32_t a)
        {
            float v;
            std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint32_t a, float v) { std::memcpy(g_rdram + (a & kRamMask), &v, 4); }

        float wrap(float a)
        {
            a = std::fmod(a, kTwoPi);
            return a < 0.0f ? a + kTwoPi : a;
        }

        void setState(State s)
        {
            g_state = s;
            g_stateShown.store(static_cast<int>(s), std::memory_order_relaxed);
        }

        void pullOff(bool off)
        {
            if (off == g_pullOff)
                return;
            for (int i = 0; i < kLcs.classes; ++i)
            {
                const uint32_t at = kLcs.pullCap + i * 0x3Cu;
                if (off)
                    g_pullCaps[i] = rdf(at);
                wrf(at, off ? 0.0f : g_pullCaps[i]);
            }
            g_pullOff = off;
        }

        float pitchOf(uint32_t at, const float p[3])
        {
            const float ox = rdf(at) - p[0], oy = rdf(at + 4) - p[1], oz = rdf(at + 8) - p[2];
            return std::atan2(-oz, std::sqrt(ox * ox + oy * oy));
        }

        void turnCamera(uint32_t cam, const float p[3], float turn, float tilt)
        {
            if (turn != 0.0f)
            {
                wrf(cam + kLcs.camBeta, wrap(rdf(cam + kLcs.camBeta) + turn));
                wrf(kLcs.lastBeta, wrap(rdf(kLcs.lastBeta) + turn));
                const float c = std::cos(turn), s = std::sin(turn);
                for (uint32_t off : kLcs.camPoints)
                {
                    const uint32_t at = cam + off;
                    const float ox = rdf(at) - p[0], oy = rdf(at + 4) - p[1];
                    wrf(at, p[0] + ox * c - oy * s);
                    wrf(at + 4, p[1] + ox * s + oy * c);
                }
            }
            if (tilt != 0.0f)
            {
                wrf(cam + kLcs.camAlpha, rdf(cam + kLcs.camAlpha) + tilt);
                wrf(kLcs.lastAlpha, rdf(kLcs.lastAlpha) + tilt);
                for (uint32_t off : kLcs.camPoints)
                {
                    const uint32_t at = cam + off;
                    const float ox = rdf(at) - p[0], oy = rdf(at + 4) - p[1], oz = rdf(at + 8) - p[2];
                    const float h = std::sqrt(ox * ox + oy * oy), d = std::sqrt(h * h + oz * oz);
                    if (h < 1e-4f)
                        continue;
                    const float e = std::clamp(std::atan2(-oz, h) + tilt, -1.4f, 1.4f);
                    const float k = d * std::cos(e) / h;
                    wrf(at, p[0] + ox * k);
                    wrf(at + 4, p[1] + oy * k);
                    wrf(at + 8, p[2] - d * std::sin(e));
                }
            }
        }

        float behind(uint32_t vehicle)
        {
            float x = rdf(vehicle + kLcs.moveSpeed), y = rdf(vehicle + kLcs.moveSpeed + 4);
            if (rd32(vehicle + kLcs.vehicleType) == kLcs.boatType || std::sqrt(x * x + y * y) <= 0.02f)
            {
                x = rdf(vehicle + kLcs.entityForward);
                y = rdf(vehicle + kLcs.entityForward + 4);
            }
            return wrap(std::atan2(-y, -x));
        }

        bool spraying(uint32_t vehicle)
        {
            if (!kLcs.cannons)
                return false;
            for (int i = 0; i < kCannonCount; ++i)
                if (rd32(kLcs.cannons + i * kCannonSize) == vehicle)
                    return true;
            return false;
        }

        void reset(uint32_t vehicle)
        {
            pullOff(false);
            setState(State::Idle);
            g_vehicle = vehicle;
            if (vehicle)
                for (int i = 0; i < 3; ++i)
                    g_lastPos[i] = rdf(vehicle + kLcs.entityPos + i * 4);
        }

        float returnDelay()
        {
            const int v = settings::value("mouse.car_return"); // Keyboard > Car camera return (vcs_input.cpp's)
            return v < 0 ? -1.0f : v / 10.0f;
        }

        void prepare(uint32_t cam)
        {
            const game::Addrs &A = game::addrs();
            const bool stick = g_stick;
            const float turn = stick ? 0.0f : g_turn, tilt = stick ? 0.0f : g_tilt;
            g_turn = g_tilt = 0.0f;
            g_stick = false;
            uint16_t mode;
            std::memcpy(&mode, g_rdram + ((cam + kLcs.camMode) & kRamMask), 2);
            const uint32_t vehicle = rd32(cam + kLcs.camTargetEntity);
            const uint32_t camera = A.world.theCamera;
            const bool usable = cam == camera + kLcs.cams + g_rdram[(camera + kLcs.activeCam) & kRamMask] * kLcs.camSize &&
                                (mode == 18 || mode == 22) && vehicle && rd32(cam + kLcs.camControl) == 3 &&
                                rd32(camera + kLcs.lookBehind) == 0;
            const uint32_t frame = rd32(A.timer.frameCounter);
            const bool gap = frame - g_frame > 1;
            g_frame = frame;
            if (!usable || vehicle != g_vehicle || gap || stick)
            {
                reset(usable ? vehicle : 0);
                if (!usable)
                    return;
            }

            const float dt = std::clamp(rdf(A.timer.timeStep), 0.0f, 5.0f) / 50.0f;
            float moved[3];
            for (int i = 0; i < 3; ++i)
            {
                const float now = rdf(vehicle + kLcs.entityPos + i * 4);
                moved[i] = now - g_lastPos[i];
                g_lastPos[i] = now;
            }
            const float p[3] = {rdf(cam + kLcs.camTarget) + moved[0], rdf(cam + kLcs.camTarget + 4) + moved[1],
                                rdf(cam + kLcs.camTarget + 8) + moved[2]};

            if (turn != 0.0f || tilt != 0.0f)
            {
                if (g_state != State::Hold)
                    g_pitch = pitchOf(cam + kLcs.camPoints[0], p);
                setState(State::Hold);
                g_idle = 0.0f;
            }
            else if (g_state != State::Idle && spraying(vehicle))
            {
                setState(State::Hold);
                g_idle = 0.0f;
            }
            else if (g_state == State::Hold)
            {
                g_idle += dt;
                const float delay = returnDelay();
                const bool tank = g_tankHold && frame - g_tankFrame <= 2u; // its turret is on the camera: kept
                if (delay >= 0.0f && g_idle >= delay && !tank)
                {
                    setState(State::Return);
                    g_returning = 0.0f;
                }
            }

            if (g_state == State::Idle)
            {
                pullOff(false);
                return;
            }
            pullOff(true);
            for (uint32_t off : kLcs.camPoints)
                for (int i = 0; i < 3; ++i)
                    wrf(cam + off + i * 4, rdf(cam + off + i * 4) + moved[i]);
            wrf(cam + kLcs.camHeadingSpeed, 0.0f);
            wrf(cam + kLcs.camPitchSpeed, 0.0f);
            if (g_state == State::Hold)
                turnCamera(cam, p, turn, tilt);
            else
            {
                g_returning += dt;
                const float k = 1.0f - std::exp(-dt / kReturnTime), cap = kReturnRate * dt;
                const float heading = std::remainder(behind(vehicle) - rdf(cam + kLcs.camBeta), kTwoPi);
                const float pitch = g_pitch - pitchOf(cam + kLcs.camPoints[0], p);
                turnCamera(cam, p, std::clamp(heading * k, -cap, cap), std::clamp(pitch * k, -cap, cap));
                if ((std::fabs(heading) < 0.01f && std::fabs(pitch) < 0.01f) || g_returning >= kReturnLimit)
                    setState(State::Idle);
            }
        }

        PS2_GUEST_CC void followCar(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            prepare(GPR_U32(ctx, 4));
            g_orig(rdram, ctx, runtime);
        }

        // TankControl (a0 the tank), before the game's: once the mouse has turned the camera behind the player's tank
        // (this camera's Hold, until the right stick takes the camera back) the turret turns toward where the camera
        // looks, at most kTurretRate - the PC way (the stick's own turn still adds in the game's update after).
        void aimTurret(uint32_t vehicle)
        {
            g_tankHold = false;
            const game::Addrs &A = game::addrs();
            const uint32_t ped = rd32(A.world.playerPed);
            if (!ped || !rd32(ped + A.off.pedInVehicle) || rd32(ped + A.off.pedVehicle) != vehicle || g_state == State::Idle ||
                vehicle != g_vehicle)
                return;
            const uint32_t camera = A.world.theCamera;
            const uint32_t cam = camera + kLcs.cams + g_rdram[(camera + kLcs.activeCam) & kRamMask] * kLcs.camSize;
            uint16_t mode;
            std::memcpy(&mode, g_rdram + ((cam + kLcs.camMode) & kRamMask), 2);
            if ((mode != 18 && mode != 22) || rd32(cam + kLcs.camTargetEntity) != vehicle)
                return;
            // the camera's Front (+0x1C0) in the tank's frame (right +0, forward +0x10)
            const float f[3] = {rdf(cam + 0x1C0), rdf(cam + 0x1C4), rdf(cam + 0x1C8)};
            const float lx = f[0] * rdf(vehicle) + f[1] * rdf(vehicle + 4) + f[2] * rdf(vehicle + 8);
            const float ly = f[0] * rdf(vehicle + 0x10) + f[1] * rdf(vehicle + 0x14) + f[2] * rdf(vehicle + 0x18);
            if (lx * lx + ly * ly < 1e-4f)
                return;
            const float want = wrap(std::atan2(-lx, ly));
            const float cur = rdf(vehicle + kTurret);
            const float dt = std::clamp(rdf(A.timer.timeStep), 0.0f, 5.0f) / 50.0f;
            const float step = std::clamp(std::remainder(want - cur, kTwoPi), -kTurretRate * dt, kTurretRate * dt);
            wrf(vehicle + kTurret, wrap(cur + step));
            g_turretShown.store(wrap(cur + step), std::memory_order_relaxed);
            g_tankHold = true;
            g_tankFrame = rd32(A.timer.frameCounter);
        }

        PS2_GUEST_CC void tankControl(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            aimTurret(GPR_U32(ctx, 4));
            g_origTank(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_rdram = runtime.memory().getRDRAM();
        if (const char *e = std::getenv("LCS_CARCAM"); e && e[0] == '0')
        {
            std::fprintf(stderr, "[lcs] car camera: the mouse moves the right stick (LCS_CARCAM=0)\n");
            return;
        }
        g_orig = runtime.lookupFunction(kLcs.followCar);
        if (!g_orig || !game::all({game::addrs().world.theCamera, game::addrs().timer.frameCounter}))
        {
            game::logUnavailable("car camera");
            return;
        }
        runtime.replaceFunction(kLcs.followCar, &followCar);
        g_installed = true;
        // the Rhino's turret on the mouse's camera (LCS_TANK_AIM=0: the stick's only, as on the PS2)
        if (const char *e = std::getenv("LCS_TANK_AIM"); !(e && e[0] == '0'))
            if ((g_origTank = runtime.lookupFunction(kTankControl)))
                runtime.replaceFunction(kTankControl, &tankControl);
        // lcscarcam -> ok state=<idle|hold|return> turret=<the turret as the mouse last aimed it> tank=<0|1>
        ps2control::addCommand("lcscarcam", [](const std::string &, ps2control::Reply reply) {
            static const char *const names[] = {"idle", "hold", "return"};
            char tail[64];
            std::snprintf(tail, sizeof(tail), " turret=%.3f tank=%d", g_turretShown.load(std::memory_order_relaxed),
                          g_origTank ? 1 : 0);
            reply(std::string("ok state=") + names[g_stateShown.load(std::memory_order_relaxed)] + tail);
        });
    }

    bool turns(uint32_t mode) { return g_installed.load(std::memory_order_relaxed) && (mode == 18 || mode == 22); }

    void mouse(float turn, float tilt)
    {
        g_turn += turn;
        g_tilt += tilt;
    }

    void stickMoved() { g_stick = true; }
}
