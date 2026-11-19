// [lcs] See lcs_input.h. Addresses: the shared table (game/lcs_addrs.cpp) and LCS's own layout below, found by matching
// LCS's code against VCS's (internal notes: LCS address report); checked in a hidden run.
#include "game/lcs_input.h"

#include "game/game_addrs.h"
#include "game/lcs_camera.h"

#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lcs::input
{
    namespace
    {
        constexpr uint32_t kRamMask = 0x01FFFFFFu;
        constexpr uint32_t kPedState = 0x34Cu;      // u32; 0x32 driving (VCS +0x8C4 == 0x37): LCS 0x286F88, the twin of
        constexpr uint32_t kPedDriving = 0x32u;     // VCS 0x2A2E30, tests lw 0x34C == 0x32
        constexpr uint32_t kCamModeFollowPed = 4u;  // as VCS (jump table decoded)
        constexpr uint32_t kNoMode = 0xFFFFu;
        // the vehicle's radio station (u8): 6 twin radio functions map VCS's +0x2BA to +0x2A0 (e.g. VCS 0x1890C0 =
        // LCS 0x167BD0); VCS's default without a vehicle is 0xA (its "off" + 1), LCS's twin 0x167DF8 has 0xB, and the
        // station update 0x169488 folds 10..22 into 10: 0..9 the ten stations, 10 off (checked in a run: L1 steps)
        constexpr uint32_t kVehicleRadio = 0x2A0u;
        constexpr uint32_t kRadioPositions = 11u;

        uint32_t rd32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (a & kRamMask), 4);
            return v;
        }
    }

    namespace
    {
        // CPad's shake. The pad's per-frame update (0x1702A0..) sends scePadSetActDirect (0x360C80) with the large motor
        // = the pad byte +0x85 (the shake's frequency) while the halfword +0x80 (its time, ms) is above 0; the small
        // motor is always 0 there (0x3D9708 / 0x3D9709 hold the two bytes). The shake is started by
        //   0x16ECC8 StartShake(pad a0, time a1 (s16), frequency a2 (u8)): sets +0x80 / +0x85 when the time is longer
        //                   than what is left (checks the shake switch 0x3E8BA5, 0x3D5AC0 and gp+0x1554 first)
        //   0x16ED28 / 0x16EDE0  the same for a point (f12..f14): only near the camera
        // Return addresses of the callers (found by `jal` in the ELF; seen in a run with VCS_RUMBLE_LOG=1, an Infernus
        // driven 15 s from a standstill): 0x12D850 and 0x12D8F4 are the two jals (0x12D848 / 0x12D8EC) of one vehicle
        // function, a time of ~80 ms and a frequency of 48..52, a call each frame the car moves ((speed * k / mass-ish
        // + 32 or 40) & 0xFF: the road rumble) - the constant, strong shake the user found; the pad's large motor then
        // sat at 48..61 of 255 the whole drive. The one-off events (a crash: time 273, frequency 146, ra 0x1258A0 from
        // 0x125898; gunfire, explosions) are other callers and stay as they are.
        constexpr uint32_t kRoadShakeRa[] = {0x0012D850u, 0x0012D8F4u};
        constexpr uint32_t kStartShake = 0x0016ECC8u;
        constexpr uint32_t kStartShakeAt = 0x0016ED28u;
        constexpr uint32_t kStartShakeNear = 0x0016EDE0u;
        PS2Runtime::RecompiledFunction g_origShake = nullptr, g_origShakeAt = nullptr, g_origShakeNear = nullptr;
        float g_drivingShake = 1.0f; // 0..2 (the Vibration setting "Driving vibration")
        bool g_shakeLog = false;

        void logShake(const char *what, R5900Context *ctx)
        {
            std::fprintf(stderr, "[lcs-shake] %s ra %x time %d freq %u\n", what, GPR_U32(ctx, 31),
                         static_cast<int16_t>(GPR_U32(ctx, 5)), GPR_U32(ctx, 6) & 0xFFu);
        }

        PS2_GUEST_CC void startShake(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_shakeLog)
                logShake("start", ctx);
            const uint32_t ra = GPR_U32(ctx, 31);
            if (ra == kRoadShakeRa[0] || ra == kRoadShakeRa[1])
            {
                const float f = static_cast<float>(GPR_U32(ctx, 6) & 0xFFu) * g_drivingShake;
                SET_GPR_U32(ctx, 6, static_cast<uint32_t>(f > 255.0f ? 255.0f : f));
            }
            g_origShake(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void startShakeAt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_shakeLog)
                logShake("at", ctx);
            g_origShakeAt(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void startShakeNear(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_shakeLog)
                logShake("near", ctx);
            g_origShakeNear(rdram, ctx, runtime);
        }
    }

    void setDrivingShake(float scale) { g_drivingShake = scale < 0.0f ? 0.0f : scale; }

    void install(PS2Runtime &runtime)
    {
        const char *e = std::getenv("VCS_RUMBLE_LOG");
        g_shakeLog = e && e[0] == '1';
        g_origShake = runtime.lookupFunction(kStartShake);
        g_origShakeAt = runtime.lookupFunction(kStartShakeAt);
        g_origShakeNear = runtime.lookupFunction(kStartShakeNear);
        if (!g_origShake || !g_origShakeAt || !g_origShakeNear)
        {
            std::fprintf(stderr, "[lcs-input] CPad::StartShake not found: driving vibration stays as the game sets it\n");
            return;
        }
        runtime.replaceFunction(kStartShake, &startShake);
        runtime.replaceFunction(kStartShakeAt, &startShakeAt);
        runtime.replaceFunction(kStartShakeNear, &startShakeNear);
    }

    State state(const uint8_t *rdram)
    {
        State s;
        const game::Addrs &A = game::addrs();
        if (!rdram || !game::all({A.world.playerPed, A.timer.userPause, A.timer.codePause}))
            return s;
        const uint32_t ped = rd32(rdram, A.world.playerPed);
        // the vehicle pointer counts only while the ped's in-vehicle flag is set (FindPlayerVehicle 0x107278)
        s.driving = ped && rd32(rdram, ped + A.off.pedInVehicle) && rd32(rdram, ped + A.off.pedVehicle) &&
                    rd32(rdram, ped + kPedState) == kPedDriving;
        s.paused = rdram[A.timer.userPause & kRamMask] || rdram[A.timer.codePause & kRamMask];
        return s;
    }

    Radio radio(const uint8_t *rdram)
    {
        Radio r;
        const game::Addrs &A = game::addrs();
        const uint32_t ped = rdram && A.world.playerPed ? rd32(rdram, A.world.playerPed) : 0;
        r.vehicle = ped && rd32(rdram, ped + A.off.pedInVehicle) ? rd32(rdram, ped + A.off.pedVehicle) : 0;
        r.offset = kVehicleRadio;
        r.positions = kRadioPositions;
        return r;
    }

    uint32_t cameraMode(const uint8_t *rdram)
    {
        const uint32_t mode = camera::mode(rdram);
        return mode == kCamModeFollowPed ? kCamModeFollowPed : kNoMode;
    }
}
