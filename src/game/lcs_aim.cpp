// [lcs] See lcs_aim.h. The logic is game/vcs_aim.cpp's (game-internals.md 12c), kept as it is there; the addresses are
// LCS's (SLUS-21423 v1.01), found by decompiling LCS's player code (Ghidra) and matching it with VCS's.
//
// LCS's free aim, from CPlayerPed::ProcessPlayerWeapon (0x34AF20, called from the player's control 0x347030): with the
// target button held (CPad 0x171F20: R1), L3 (0x1727A8) and a weapon whose info allows it (info +0 not melee, +0x6C bit
// 0x40), not sprinting (+0x354 != 5), the free-aim flag +0xCBC = 1 and the dummy +0xCC0 (an object the player's
// constructor 0x346D20 makes) placed ahead of him, made the target (+0x6BC, through 0x1DBD90 = VCS's SetAimFlag) and
// marked white (CWeaponEffects::MarkTarget 0x2B6108). With +0xCBC set the dummy's update 0x34A4A0 moves it by the left
// stick within 50..110 deg from his up and 110 deg either side of his forward, then pulls it onto what the camera's
// line to it hits (VCS's FUN_002343E0, the same limits); the lock-on camera 0x280E28 (mode 11) drops its crosshair
// offset and pitch limits, and CWeaponEffects::Render 0x2B6250 draws the circle in the middle of the screen. With it
// clear and the dummy the target the game treats it as a lock-on to an object (red marker ignored: MarkTarget skips
// the entity already marked). Letting go of R1 clears both (ClearWeaponTarget 0x34C6A0).
#include "game/lcs_aim.h"

#include "game/game_addrs.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_control.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace lcs::aim
{
    namespace
    {
        using vcs::aim::Input;
        using vcs::aim::Output;

        constexpr uint32_t kRamMask = 0x01FFFFFFu;

        struct Addrs
        {
            // hooked functions
            uint32_t gameProcess;    // CGame::Process: scripts, the world (peds move, guns fire), then the camera
            uint32_t cameraProcess;  // CCamera::Process: after the world, before the HUD and the frame's render
            uint32_t shotLine;       // the weapons' line test (CWorld::ProcessLineOfSight's wrapper, 8 args)
            uint32_t shotLineReturn[2]; // its calls in FireInstantHit for a shooter with a target
            uint32_t movesAiming;    // the ped's weapon lets him move while aiming (a0 the ped)
            uint32_t movesAimingCamReturn; // its call in CCamera::CamControl (lock-on camera -> follow camera: blend or cut)
            uint32_t transition, transitionDurations[2]; // TheCamera: the blend between cameras running (u8), its ms
            // the ped
            uint32_t pedTarget, pedFreeAim, pedDummy, pedSlot, pedWeapons, pedState, pedInVehicle, pedMoveState;
            uint32_t entityType;     // u32 holding bits 33..35 of the u64 at +0x40: & 0xE = 6 ped, 4 vehicle, 8 object
            uint32_t weaponInfo;     // *(*(this) + 4) + type * 0x70: the weapon's info (+0 fire type, +4 range, +0x6C flags)
            // TheCamera / CCam (offsets)
            uint32_t activeCam, cams, camSize, camMode, camAlpha, camBeta, camFront, camSource;
            uint32_t camFov;         // float, degrees
            uint32_t pedAttachedTo;  // the vehicle a script attached him to (ATTACH_CHAR_TO_CAR 0x0469), 0 none
        };

        constexpr Addrs kLcs{
            0x002FBAF0u, // its only caller is the main loop 0x1F5BC0; calls CCamera::Process (0x2FBE3C) after the world,
                         // and CWaterCannons::Update 0x2CE178 (VCS 0x3BA8D8 twin: aligned 0.69)
            0x0028FAC8u, // passed TheCamera (0x43BBF0); its Cams[ActiveCam] -> CCam::Process 0x27A380 (VCS 0x378DD8)
            0x002B0BD0u, // VCS 0x2D0870's exact twin (21 instructions, shape 1.0), the line test FireInstantHit makes
            {0x002A52F8u, 0x002A53F0u}, // FireInstantHit (0x2A4BF8, VCS 0x2C3E48 aligned 0.81) for a shooter with a
                         // target: the call at 0x2A52F0 (toward it plus the spread; 0x3D9CF0 set for the player first) and
                         // the one at 0x2A53E8 (+0x33C bit 0 clear: along the hand's bone, 0x144CF0 just before); t1 = s3
                         // the shooter at both. (0x2A5748 is a special model's, 0x2A59B4 the shot without a target.)
            0x001D5190u, // VCS 0x1F1698's exact twin (13 instructions, shape 1.0)
            0x0028BE10u, // its call at 0x28BE08 in CamControl 0x2886D8 (VCS 0x372450, aligned 0.83; VCS's call 0x375B3C)
            0x120u, {0x134u, 0x138u}, // CCamera::StartTransition 0x293520 (VCS 0x37C9C8, aligned 0.94): VCS's sb +0x8D0,
                         // sw +0x8DC / +0x8E0 map to +0x120, +0x134 / +0x138 (the start time +0x8D8 -> +0x130)
            0x6BCu,      // FireInstantHit's target (VCS +0x820); 0x1DBD90 sets it with RegisterReference
            0xCBCu,      // ProcessPlayerWeapon: = 1 when R1 + L3 enter free aim; read by the dummy's update, the lock-on
                         // camera, FireInstantHit (no spread) and CWeaponEffects::Render (VCS bit 51 of +0xCC8)
            0xCC0u,      // the dummy: made in the player's constructor 0x346D20, placed and made the target on entry
            0x6B8u,      // s8 current slot (VCS +0x789): every weapon read is +0x594 + slot * 0x1C
            0x594u,      // the weapon slots, type first (VCS +0x578)
            0x34Cu,      // u32 state, 0x32 driving (lcs_input.cpp)
            0x538u,      // in-vehicle flag (FindPlayerVehicle 0x107278)
            0x354u,      // 5 sprinting: ProcessPlayerWeapon enters no free aim then (VCS: "sprinting he never gets there")
            0x44u,       // `(+0x40 & 0xE00000000) == 0x600000000` for peds, 0x800000000 objects (FireInstantHit, Render)
            0x003D9B48u, // FUN_0034F2F0(type) = *(*(0x3D9B48) + 4) + type * 0x70 (VCS 0x712770, same stride)
            0x10Cu, 0x220u, 0x2B0u, 0x40u, 0xE0u, 0xF0u, // as lcs_camera.cpp
            0x1C0u,      // Front: the lock-on camera 0x280E28 stores (-cos b cos a, -sin b cos a, sin a) there
            0x1D0u,      // Source: the same camera's line test runs from its target point to here (VCS +0x20)
            0xE8u,       // as lcs_camera.cpp: clamped to 70 in the scope update 0x27D440 (VCS +0x134)
            0x760u,      // as lcs_camera.cpp: 7 aligned reads of the camera's ped in the scope update 0x27D440, which
                         // runs mode 45 too (VCS +0x858); the attach 0x1F08D0 stores the vehicle there (mountgun notes)
        };
        const Addrs &A = kLcs;

        constexpr uint32_t kCamModeLockOn = 11; // ProcessPlayerWeapon asks for it (0x2930F0(TheCamera, 0xB)) locked on
        // a gun on a vehicle: the camera the attach (0x0469 -> 0x1F08D0) puts him in, one of the scope update 0x27D440's
        // cases (lcs_camera.cpp; seen in missions 42 / 43 / 44, mountgun notes 2026-11-18; VCS's mode 45 too)
        constexpr uint32_t kCamModeMountedGun = 45;
        constexpr uint32_t kStateDriving = 0x32u;
        constexpr uint32_t kMoveSprint = 5u;
        // first-person weapons: R1 puts them in a scope camera (ProcessPlayerWeapon: 30 -> mode 8, 28 / 29 -> 7,
        // 36 -> 46), free aim is not theirs
        bool firstPerson(uint32_t type) { return type == 28 || type == 29 || type == 30 || type == 36; }
        // Guest memory for the shot's start point: the EE kernel's area below the game; LCS's runtime heap is at
        // 0x80000-0xC0000 (lcs_overrides.cpp), this is below it, as VCS's (vcs_aim.cpp).
        constexpr uint32_t kScratch = 0x00060000u;

        // as vcs_aim.cpp
        constexpr float kDummyDistance = 12.0f;
        constexpr float kPivotHeight = 0.6f;
        constexpr float kPitchLimit = 1.2f;
        constexpr float kStartPitchLimit = 0.35f;
        constexpr float kStickRate = 2.0f;
        constexpr uint32_t kBlendInMs = 100, kBlendOutMs = 200; // vcs_aim.cpp's: the blend into / out of free aim

        PS2Runtime::RecompiledFunction g_origGame = nullptr, g_origCamera = nullptr, g_origShotLine = nullptr,
                                       g_origMovesAiming = nullptr;
        uint8_t *g_rdram = nullptr;

        enum class State
        {
            Idle,
            Entering,
            Active,
            Released,
            Held,
            Mounted, // a gun on a vehicle a script attached him to: its camera is ours (vcs_aim.cpp's Mounted)
        };
        bool g_mountedPad = true; // LCS_MOUNTED_PAD=0: the gun on a vehicle left to the game and lcs_camera.cpp's mouse
        State g_state = State::Idle; // game thread
        int g_wait = 0;
        constexpr int kRetryFrames = 8;
        uint32_t g_dummy = 0;
        float g_yaw = 0, g_pitch = 0;
        int g_quickBlend = 0;            // frames left to shorten the camera's blend into / out of free aim
        uint32_t g_blendMs = kBlendInMs; // to this
        int g_handBack = 0;              // frames after free aim ended: the camera's way back blends for every gun
        std::atomic<bool> g_active{false};
        std::atomic<uint32_t> g_shots{0};
        std::atomic<int> g_stateOut{0};

        bool inRam(uint32_t a) { return a >= 0x00100000u && (a & kRamMask) + 4 <= 0x02000000u; }
        uint32_t rd32(uint32_t a)
        {
            uint32_t v = 0;
            if (inRam(a))
                std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        void wr32(uint32_t a, uint32_t v)
        {
            if (inRam(a))
                std::memcpy(g_rdram + (a & kRamMask), &v, 4);
        }
        float rdf(uint32_t a)
        {
            float v = 0.0f;
            if (inRam(a))
                std::memcpy(&v, g_rdram + (a & kRamMask), 4);
            return v;
        }
        void wrf(uint32_t a, float v)
        {
            if (inRam(a))
                std::memcpy(g_rdram + (a & kRamMask), &v, 4);
        }
        void wrVec(uint32_t a, const float v[3])
        {
            for (int i = 0; i < 3; ++i)
                wrf(a + i * 4, v[i]);
        }

        uint32_t playerPed() { return rd32(game::addrs().world.playerPed); }
        uint32_t activeCam()
        {
            const uint32_t camera = game::addrs().world.theCamera;
            const uint8_t i = g_rdram[(camera + A.activeCam) & kRamMask];
            return camera + A.cams + (i < 3 ? i : 0) * A.camSize;
        }
        uint32_t camMode(uint32_t cam)
        {
            uint16_t m;
            std::memcpy(&m, g_rdram + ((cam + A.camMode) & kRamMask), 2);
            return m;
        }
        bool isObject(uint32_t e) { return inRam(e) && (rd32(e + A.entityType) & 0xEu) == 8u; }
        bool freeAimFlag(uint32_t ped) { return rd32(ped + A.pedFreeAim) != 0; }
        void setFreeAimFlag(uint32_t ped, bool on) { wr32(ped + A.pedFreeAim, on ? 1u : 0u); }
        bool onFoot(uint32_t ped) { return ped && !rd32(ped + A.pedInVehicle) && rd32(ped + A.pedState) != kStateDriving; }

        uint32_t weaponType(uint32_t ped)
        {
            const int8_t slot = static_cast<int8_t>(g_rdram[(ped + A.pedSlot) & kRamMask]);
            return slot >= 0 && slot < 10 ? rd32(ped + A.pedWeapons + slot * 0x1Cu) : 0;
        }
        uint32_t weaponInfo(uint32_t type)
        {
            const uint32_t table = rd32(rd32(A.weaponInfo) + 4);
            return table && type < 64 ? table + type * 0x70u : 0;
        }
        // The game's own test for free aim (ProcessPlayerWeapon): not melee (info +0), and +0x6C bit 0x40; and not a
        // first-person weapon (its R1 is the scope). Fire type 1 is the instant-hit guns (FireInstantHit), whose shots
        // go through the circle; thrown weapons (2) aim the throw (the game's free aim with them).
        bool isGun(uint32_t type)
        {
            const uint32_t info = weaponInfo(type);
            return info && type && !firstPerson(type) && rd32(info) == 1u && (rd32(info + 0x6C) & 0x40u);
        }
        float weaponRange(uint32_t ped)
        {
            const uint32_t info = weaponInfo(weaponType(ped));
            const float range = info ? rdf(info + 4) : 0.0f;
            return range >= 5.0f && range <= 200.0f ? range : 30.0f;
        }

        void dummyPlace(uint32_t ped)
        {
            const float cp = std::cos(g_pitch);
            const float at[3] = {rdf(ped + 0x30) + kDummyDistance * cp * std::cos(g_yaw),
                                 rdf(ped + 0x34) + kDummyDistance * cp * std::sin(g_yaw),
                                 rdf(ped + 0x38) + kPivotHeight + kDummyDistance * std::sin(g_pitch)};
            wrVec(g_dummy + 0x30, at);
        }

        void aimAlongCamera(uint32_t cam)
        {
            const float fx = rdf(cam + A.camFront), fy = rdf(cam + A.camFront + 4), fz = rdf(cam + A.camFront + 8);
            if (fx == 0.0f && fy == 0.0f)
                return;
            g_yaw = std::atan2(fy, fx);
            g_pitch = std::clamp(std::asin(std::clamp(fz, -1.0f, 1.0f)), -kStartPitchLimit, kStartPitchLimit);
        }

        void stop(const char *why)
        {
            if (g_state == State::Active)
            {
                std::fprintf(stderr, "[lcs-aim] free aim off (%s)\n", why);
                g_handBack = 30;
                g_quickBlend = 10;
                g_blendMs = kBlendOutMs;
            }
            g_state = State::Idle;
            g_active = false;
        }

        // CGame::Process: the world runs with the free-aim flag cleared - to the game a lock-on to an object.
        PS2_GUEST_CC void gameProcess(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_state == State::Active)
                if (const uint32_t ped = playerPed())
                    setFreeAimFlag(ped, false);
            g_origGame(rdram, ctx, runtime);
        }

        // CCamera::Process (inside CGame::Process, after the world): the camera and then the HUD see free aim again.
        PS2_GUEST_CC void cameraProcess(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_state == State::Active)
                if (const uint32_t ped = playerPed(); ped && rd32(ped + A.pedTarget) == g_dummy)
                    setFreeAimFlag(ped, true);
            // the blend into / out of free aim cut to 100 / 200 ms (vcs_aim.cpp: a PC shooter's aim comes up at once)
            if (g_quickBlend > 0)
            {
                --g_quickBlend;
                const uint32_t camera = game::addrs().world.theCamera;
                if (rdram[(camera + A.transition) & kRamMask])
                    for (const uint32_t at : A.transitionDurations)
                        wr32(camera + at, std::min(rd32(camera + at), g_blendMs));
            }
            g_origCamera(rdram, ctx, runtime);
        }

        // Does the ped's weapon let him move while aiming? Asked by CamControl when the lock-on camera goes back to the
        // follow camera: no cuts, yes blends. Leaving free aim it is yes for the camera, so every gun's aim ends in the
        // same blend (vcs_aim.cpp's movesAiming).
        PS2_GUEST_CC void movesAiming(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_handBack > 0 && GPR_U32(ctx, 31) == A.movesAimingCamReturn)
            {
                SET_GPR_U64(ctx, 2, 1);
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_origMovesAiming(rdram, ctx, runtime);
        }

        // The weapons' line test (from a0 to a1; t1 the shooter): for the player's free-aim shot the line is the
        // camera's through the circle (vcs_aim.cpp's shotLine).
        PS2_GUEST_CC void shotLine(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ped = playerPed();
            const uint32_t ra = GPR_U32(ctx, 31);
            if (g_state == State::Active && (ra == A.shotLineReturn[0] || ra == A.shotLineReturn[1]) && ped &&
                GPR_U32(ctx, 9) == ped && rd32(ped + A.pedTarget) == g_dummy)
            {
                const uint32_t from = GPR_U32(ctx, 4), to = GPR_U32(ctx, 5), cam = activeCam();
                const float src[3] = {rdf(cam + A.camSource), rdf(cam + A.camSource + 4), rdf(cam + A.camSource + 8)};
                const float gun[3] = {rdf(from), rdf(from + 4), rdf(from + 8)};
                const float end[3] = {rdf(to), rdf(to + 4), rdf(to + 8)};
                float dir[3] = {rdf(g_dummy + 0x30) - src[0], rdf(g_dummy + 0x34) - src[1], rdf(g_dummy + 0x38) - src[2]};
                const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
                const float range = std::sqrt((end[0] - gun[0]) * (end[0] - gun[0]) + (end[1] - gun[1]) * (end[1] - gun[1]) +
                                              (end[2] - gun[2]) * (end[2] - gun[2]));
                if (len > 0.01f && range > 0.01f)
                {
                    for (float &d : dir)
                        d /= len;
                    const float along = std::max(0.0f, (gun[0] - src[0]) * dir[0] + (gun[1] - src[1]) * dir[1] +
                                                           (gun[2] - src[2]) * dir[2]);
                    const float start[3] = {src[0] + dir[0] * along, src[1] + dir[1] * along, src[2] + dir[2] * along};
                    const float stop[3] = {start[0] + dir[0] * range, start[1] + dir[1] * range, start[2] + dir[2] * range};
                    wrVec(kScratch, start);
                    wr32(kScratch + 12, 0);
                    wrVec(to, stop);
                    SET_GPR_U32(ctx, 4, kScratch);
                    g_shots.fetch_add(1, std::memory_order_relaxed);
                }
            }
            g_origShotLine(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_rdram = runtime.memory().getRDRAM();
        if (const char *e = std::getenv("VCS_FREEAIM"); e && e[0] == '0')
        {
            std::fprintf(stderr, "[lcs-aim] free aim off (VCS_FREEAIM=0)\n");
            return;
        }
        if (const char *e = std::getenv("LCS_MOUNTED_PAD"); e && e[0] == '0')
        {
            g_mountedPad = false;
            std::fprintf(stderr, "[lcs-aim] guns on vehicles left to the game (LCS_MOUNTED_PAD=0)\n");
        }
        g_origGame = runtime.lookupFunction(A.gameProcess);
        g_origCamera = runtime.lookupFunction(A.cameraProcess);
        g_origShotLine = runtime.lookupFunction(A.shotLine);
        g_origMovesAiming = runtime.lookupFunction(A.movesAiming);
        if (!g_origGame || !g_origCamera || !g_origShotLine || !g_origMovesAiming)
        {
            std::fprintf(stderr, "[lcs-aim] a hooked function is missing: free aim off\n");
            g_origGame = nullptr;
            return;
        }
        runtime.replaceFunction(A.gameProcess, &gameProcess);
        runtime.replaceFunction(A.cameraProcess, &cameraProcess);
        runtime.replaceFunction(A.shotLine, &shotLine);
        runtime.replaceFunction(A.movesAiming, &movesAiming);
        // lcsaim -> ok active=<0|1> state=<n> shots=<n> (and the player's free-aim flag, target, dummy)
        ps2control::addCommand("lcsaim", [](const std::string &, ps2control::Reply reply) {
            const uint32_t ped = g_rdram ? playerPed() : 0;
            char text[160];
            std::snprintf(text, sizeof(text), "ok active=%d state=%d shots=%u flag=%u target=0x%x dummy=0x%x gun=%d",
                          g_active.load() ? 1 : 0, g_stateOut.load(), g_shots.load(), ped ? rd32(ped + A.pedFreeAim) : 0,
                          ped ? rd32(ped + A.pedTarget) : 0, ped ? rd32(ped + A.pedDummy) : 0, gunInHand() ? 1 : 0);
            reply(text);
        });
        std::fprintf(stderr, "[lcs-aim] free aim hooks installed\n");
    }

    bool gunInHand()
    {
        const uint32_t ped = g_rdram ? playerPed() : 0;
        return ped && isGun(weaponType(ped));
    }

    bool aiming() { return g_active.load(std::memory_order_relaxed); }

    namespace
    {
        // Attached to a vehicle by a script (0x0469: missions 42 / 43 / 44 at their start, SALS3 / RAYS1 / SALH5 later)
        // and behind its gun: the camera (mode 45, the scope update 0x27D440) is the aim, its heading +0xF0 the turn
        // from the side he faces, signed, kept by the update within the attachment's limit, its pitch +0xE0 within the
        // script's. The game turns it by the left stick and fires on Circle (ProcessPlayerWeapon 0x34AF20 -> CPad
        // 0x171A28); its right stick barely moves it and R1 does nothing (mountgun notes). As vcs_aim.cpp's mounted():
        // the right stick turns it (2 rad/s at full tilt, on a curve) and the gamepad's R1 fires (Circle, the right
        // thumb being on the stick). The mouse turns it as lcs_camera.cpp's scope path does (the same sum: 1:1, slower
        // zoomed in), since that path is skipped while this one has the camera. The left stick still turns it, as on
        // the PS2 (VCS centres it; here nothing else reads it on the gun).
        bool mountedGun(uint32_t ped)
        {
            return g_mountedPad && ped && rd32(ped + A.pedAttachedTo) && camMode(activeCam()) == kCamModeMountedGun;
        }

        Output mounted(uint32_t ped, const Input &in)
        {
            Output out;
            if (g_state != State::Mounted)
            {
                if (g_state == State::Active)
                    stop("a gun on a vehicle");
                g_state = State::Mounted;
                g_active = true;
                std::fprintf(stderr, "[lcs-aim] a gun on a vehicle (attached to 0x%x)\n", rd32(ped + A.pedAttachedTo));
            }
            out.camera = out.padR1Fires = true;
            const uint32_t cam = activeCam();
            float fov = rdf(cam + A.camFov);
            fov = fov >= 1.0f && fov <= 130.0f ? fov : 70.0f;
            const float k = fov / 70.0f;
            const float dt = std::clamp(rdf(game::addrs().timer.timeStep), 0.05f, 3.4f) / 50.0f;
            const float turn = in.turn * k + in.turnRate * kStickRate * dt;
            const float tilt = in.tilt * k + in.tiltRate * kStickRate * dt;
            // signed (lcs_camera.cpp / vcs_aim.cpp: wrapped into 0..2 pi a turn right of the middle became the limit on
            // the left)
            if (turn != 0.0f)
                wrf(cam + A.camBeta, std::remainder(rdf(cam + A.camBeta) + turn, 6.2831855f));
            if (tilt != 0.0f)
                wrf(cam + A.camAlpha, rdf(cam + A.camAlpha) + tilt);
            return out;
        }
    }

    Output update(const Input &in)
    {
        Output out;
        const uint32_t ped = g_rdram && g_origGame ? playerPed() : 0;
        struct Publish
        {
            ~Publish() { g_stateOut = static_cast<int>(g_state); }
        } publish;
        if (g_handBack > 0)
            --g_handBack;
        // Gamepad > Free aim: the gamepad's free-aim way on the gun (off: the game's, the mouse by lcs_camera.cpp)
        if (in.pad && mountedGun(ped))
            return mounted(ped, in);
        if (g_state == State::Mounted)
        {
            std::fprintf(stderr, "[lcs-aim] the gun on a vehicle off\n");
            g_state = State::Idle;
            g_active = false;
        }
        if (!ped || !in.want || !onFoot(ped))
        {
            stop(!in.want ? "released" : "not on foot");
            return out;
        }
        if (!gunInHand())
        {
            if (g_state == State::Idle)
                return out;
            stop("no gun in hand");
            g_state = State::Held;
            out.own = true;
            return out;
        }
        out.own = true;
        const uint32_t cam = activeCam();
        switch (g_state)
        {
        case State::Released:
            if (--g_wait <= 0)
                g_state = State::Idle;
            return out;
        case State::Held:
        case State::Idle:
            aimAlongCamera(cam);
            g_state = State::Entering;
            g_wait = 0;
            g_quickBlend = 30; // until a few frames after the free-aim lock is there
            g_blendMs = kBlendInMs;
            g_handBack = 0;
            out.r1 = out.l3 = true;
            return out;
        case State::Entering:
        {
            const uint32_t target = rd32(ped + A.pedTarget);
            if (!(isObject(target) && target == rd32(ped + A.pedDummy) && freeAimFlag(ped)))
            {
                // not yet (busy, or sprinting: the game enters no free aim then); a ped locked on, or 20 frames:
                // let go a few frames and try again
                aimAlongCamera(cam);
                out.r1 = !target && ++g_wait < 20 && rd32(ped + A.pedMoveState) != kMoveSprint;
                out.l3 = out.r1;
                if (!out.r1)
                {
                    g_state = State::Released;
                    g_wait = kRetryFrames;
                }
                return out;
            }
            g_dummy = target;
            g_state = State::Active;
            g_active = true;
            std::fprintf(stderr, "[lcs-aim] free aim on (dummy 0x%x, %d frame(s) after R1 + L3)\n", target, g_wait + 1);
            break;
        }
        case State::Active:
            if (rd32(ped + A.pedTarget) != g_dummy)
            {
                stop("lock lost");
                g_state = State::Released;
                g_wait = kRetryFrames;
                return out;
            }
            break;
        }
        out.camera = true;

        // turn the aim, and the lock-on camera by the same amount before its update (vcs_aim.cpp)
        const float dt = std::clamp(rdf(game::addrs().timer.timeStep), 0.05f, 3.4f) / 50.0f;
        const float turn = in.turn + in.turnRate * kStickRate * dt;
        const float pitch = std::clamp(g_pitch + in.tilt + in.tiltRate * kStickRate * dt, -kPitchLimit, kPitchLimit);
        const float tilt = pitch - g_pitch;
        g_yaw = std::remainder(g_yaw + turn, 6.2831855f);
        g_pitch = pitch;
        if (camMode(cam) == kCamModeLockOn && (turn != 0.0f || tilt != 0.0f))
        {
            const float beta = std::fmod(rdf(cam + A.camBeta) + turn, 6.2831855f);
            wrf(cam + A.camBeta, beta < 0.0f ? beta + 6.2831855f : beta);
            wrf(cam + A.camAlpha, rdf(cam + A.camAlpha) + tilt);
        }
        dummyPlace(ped);
        out.r1 = true;
        return out;
    }
}
