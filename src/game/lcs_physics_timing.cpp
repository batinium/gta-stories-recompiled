// [lcs] See lcs_physics_timing.h. Vehicle physics that LCS applies a fixed amount every frame, put on the time step
// as game/vcs_physics_timing.cpp does for VCS (docs/game-internals.md 4): scaled by the frame's step over the PS2's
// (50/30) when the frame is shorter, only while the frame isn't the PS2's (independent timing, or original at 60).
//
// Rolling resistance. CVehicle::ProcessWheel (FUN_00327C40 cars - VCS 0x3E6C18, best.py 0.82 -, FUN_003285A8 bikes)
// with neither gas nor brake (and a wheel speed under 0.01) brakes the wheel by
// `mod_HandlingManager.fWheelFriction (*0x3D0238 + 4) * k / mass` (k 1, 0.1 under 500 kg, 0.2 for the RC Bandit
// (model 0xA9), 0.6 / (mass + 200) for vehicle type 5) and clamps the wheel's forward correction with it - per frame,
// never times the step (reVC's FIX_BUGS line `brake *= CTimer::GetTimeStepFix()`): at 60 fps a coasting car slowed
// twice as fast as at 30. Unlike VCS (whose constants are inline, patched in overrides.toml) LCS reads the friction
// from the handling manager's header, so the hooks write it scaled before every call. Its only readers are these two
// functions (the handling manager's other readers index its entries at +0x20 + n x 0xF0); the original value is taken
// on the first call and written back unscaled at 30 fps and below - the PS2's value bit for bit.
//
// A bike's brake wobble (VCS 3., GitHub issue 64). CBike::ProcessControl (0x157990), braking hard with the brake leading
// the gas by more than 0.9: a phase (+0x5D0) grows by random * 0.2 * step, and the bike is pushed sideways once a frame,
// ApplyTurnForce 0x2BC980 (min(speed, 0.2) * sin(phase) * turn mass * 0.03 / 0.007 * right) - the call at 0x15BF04, VCS's
// 0x33BE54 instruction for instruction. Scaled as VCS's (f12-f14 at return address 0x15BF0C).
//
// The fire truck's water cannon (VCS 5., GitHub issue 144). CWaterCannon (3 at 0x60CFB0, 0x250 bytes, VCS's layout: +0
// vehicle, +4 s16 newest of 16 points, +8 set-up time, +0x210 in use): Update_OncePerFrame 0x2CD310 (VCS 0x186AA8: the
// same `set-up + 150 < now` test, then a new point every frame) - the water lived 16 frames, half a second at 30 fps.
// As VCS: the trail moves on once per 1/30 s of game time, by writing the set-up time as now on the frames between.
//
// Not found in LCS: VCS's push from a body under a wheel (FUN_003F34F0: no twin, best.py 0.42); the rotor blades' test
// (VCS FUN_003EE308: no LCS twin looked for - LCS has no helicopter the player flies).
//
// VCS_PHYSICS_TIMESTEP=0 keeps the game's per-frame amounts (as for VCS).
#include "game/lcs_physics_timing.h"

#include "game/game_table.h"
#include "game/vcs_framerate.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lcs::physics_timing
{
    namespace
    {
        struct Addrs
        {
            uint32_t wheelCar;        // fn: CVehicle::ProcessWheel (cars)
            uint32_t wheelBike;       // fn: the bikes' ProcessWheel
            uint32_t handlingManager; // pointer: mod_HandlingManager; +4 fWheelFriction
            uint32_t timeStep;        // CTimer::ms_fTimeStep
            uint32_t applyTurnForce;  // fn: CPhysical::ApplyTurnForce(f12-f14 force, f15-f17 point, a0)
            uint32_t brakeWobbleRa;   // return address of CBike::ProcessControl's brake wobble push
            uint32_t cannonUpdate;    // fn: CWaterCannon::Update_OncePerFrame(cannon a0, index a1)
            uint32_t cannons;         // CWaterCannon[3], 0x250 bytes each
            uint32_t frameCounter;    // CTimer::m_FrameCounter
            uint32_t timeMs;          // CTimer::m_snTimeInMilliseconds
        };
        constexpr Addrs kLcs{
            0x00327C40u, // best.py twin of VCS 0x3E6C18 (0.82); args as VCS's (f12 thrust, f13 brake, f14 adhesion x step
                         // at 0x327D34); rolling resistance at 0x327ED4..0x327FB8
            0x003285A8u, // the same three reads of the friction (0x328868 / 0x3288C0 / 0x3288F4)
            0x003D0238u, // `lui 0x3D; lw 0x238; lwc1 4(...)` in both, then `/ mass` (FUN_001449A0)
            0x003D9B8Cu, // game_addrs timer.timeStep
            0x002BC980u, // world COM (0x2D7FB0 with +0xF0), arm = f15-17 - COM, cross with f12-14 (0x2D7F40), / turn
                         // mass +0xD4 into the turn speed +0x80 (clamped +-4): VCS 0x347E78
            0x0015BF0Cu, // jal 0x2BC980 at 0x15BF04 after `lui 0x3BE5; ori 0x6042` (0.007) / 0x3CF5C28F (0.03),
                         // f15-f17 = f20 x the matrix's right row (VCS 0x33BE54 / 0x33BE5C, the same instructions)
            0x002CD310u, // lw 0x8(s1); addiu 0x96; sltu with the time ms; lh +4 + 1 mod 16; sw zero 0x210 (VCS 0x186AA8)
            0x0060CFB0u, // its only caller 0x2CE178: s0 = 0x610000 - 0x3050, += 0x250, slti 3
            0x003D9B6Cu, // game_addrs timer.frameCounter
            0x003D9B70u, // game_addrs timer.timeMs
        };
        constexpr uint32_t kCannonSize = 0x250u, kCannonCount = 3u;
        constexpr float kPs2Step = 50.0f / 30.0f;

        PS2Runtime::RecompiledFunction g_origCar = nullptr, g_origBike = nullptr, g_origTurnForce = nullptr,
                                       g_origCannon = nullptr;
        bool g_haveFriction = false;
        float g_friction = 0.0f; // the handling file's fWheelFriction

        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & 0x01FFFFFFu), 4);
            return v;
        }
        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & 0x01FFFFFFu), 4);
            return v;
        }

        bool active()
        {
            return vcs::timing() == vcs::Timing::Independent || vcs::frameCap() != 30;
        }

        void scaleFriction(uint8_t *rdram)
        {
            const uint32_t manager = rd32(rdram, kLcs.handlingManager);
            if (manager < 0x100000u || manager >= 0x2000000u)
                return;
            const uint32_t at = manager + 4u;
            if (!g_haveFriction)
            {
                g_friction = rdf(rdram, at);
                g_haveFriction = true;
                std::fprintf(stderr, "[lcs-physics] wheel friction %g (rolling resistance on the time step)\n", g_friction);
            }
            float scale = 1.0f;
            if (active())
            {
                const float step = rdf(rdram, kLcs.timeStep);
                if (step >= 0.0f && step < kPs2Step)
                    scale = step / kPs2Step;
            }
            const float v = g_friction * scale;
            std::memcpy(rdram + (at & 0x01FFFFFFu), &v, 4);
        }

        // the frame's step over the PS2's when it is shorter, else 1 (the game's own amount)
        float frameScale(const uint8_t *rdram)
        {
            if (!active())
                return 1.0f;
            const float step = rdf(rdram, kLcs.timeStep);
            return step >= 0.0f && step < kPs2Step ? step / kPs2Step : 1.0f;
        }

        PS2_GUEST_CC void applyTurnForce(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (GPR_U32(ctx, 31) == kLcs.brakeWobbleRa)
            {
                const float scale = frameScale(rdram);
                ctx->f[12] *= scale;
                ctx->f[13] *= scale;
                ctx->f[14] *= scale;
            }
            g_origTurnForce(rdram, ctx, runtime);
        }

        // per cannon, the steps since its trail last moved on, and the set-up time the game wrote (Init writes it anew
        // when a cannon is taken again, which is how a new one is recognised). Game thread only. (vcs_physics_timing 5.)
        struct CannonClock
        {
            bool known = false;
            uint32_t vehicle = 0, setUp = 0, written = 0, frame = 0;
            float sum = 0.0f;
        };
        CannonClock g_cannons[kCannonCount];

        void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & 0x01FFFFFFu), &v, 4); }

        PS2_GUEST_CC void cannonUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t cannon = GPR_U32(ctx, 4);
            const uint32_t n = (cannon - kLcs.cannons) / kCannonSize;
            if (cannon >= kLcs.cannons && n < kCannonCount && (cannon - kLcs.cannons) % kCannonSize == 0u)
            {
                CannonClock &c = g_cannons[n];
                const uint32_t vehicle = rd32(rdram, cannon), setUp = rd32(rdram, cannon + 8u);
                const uint32_t frame = rd32(rdram, kLcs.frameCounter);
                if (!active())
                {
                    if (c.known && setUp == c.written) // switched off mid-trail: the game's own time back
                        wr32(rdram, cannon + 8u, c.setUp);
                    c.known = false;
                }
                else
                {
                    if (!c.known || c.vehicle != vehicle || setUp != c.written)
                        c = {true, vehicle, setUp, setUp, frame - 1u, 0.0f};
                    else if (frame - c.frame > 8u) // a gap (the game was elsewhere): the count starts again
                        c.sum = 0.0f, c.frame = frame - 1u;
                    if (c.frame != frame) // once a frame
                    {
                        const float step = rdf(rdram, kLcs.timeStep);
                        c.sum += step > 0.0f ? step : 0.0f;
                        c.frame = frame;
                        // a frame about as long as the PS2's moves on by itself, shorter ones carry
                        const bool moves = c.sum >= kPs2Step * 0.9f;
                        if (moves)
                            c.sum = std::clamp(c.sum - kPs2Step, -0.1f * kPs2Step, kPs2Step);
                        // the game moves the trail on when set-up + 150 ms < now: the real time lets its own test
                        // decide (nothing in the first 150 ms), now holds it
                        c.written = moves ? c.setUp : rd32(rdram, kLcs.timeMs);
                        wr32(rdram, cannon + 8u, c.written);
                    }
                }
            }
            g_origCannon(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void wheelCar(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            scaleFriction(rdram);
            g_origCar(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void wheelBike(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            scaleFriction(rdram);
            g_origBike(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *v = std::getenv("VCS_PHYSICS_TIMESTEP"); v && v[0] == '0')
        {
            std::fprintf(stderr, "[lcs-physics] per-frame amounts kept (VCS_PHYSICS_TIMESTEP=0)\n");
            return;
        }
        g_origCar = runtime.lookupFunction(kLcs.wheelCar);
        g_origBike = runtime.lookupFunction(kLcs.wheelBike);
        if (!g_origCar || !g_origBike)
        {
            game::logUnavailable("rolling resistance on the time step (lcs_physics_timing: no ProcessWheel)");
            return;
        }
        runtime.replaceFunction(kLcs.wheelCar, &wheelCar);
        runtime.replaceFunction(kLcs.wheelBike, &wheelBike);
        g_origTurnForce = runtime.lookupFunction(kLcs.applyTurnForce);
        if (g_origTurnForce)
            runtime.replaceFunction(kLcs.applyTurnForce, &applyTurnForce);
        else
            game::logUnavailable("the bike's brake wobble on the time step (lcs_physics_timing: no ApplyTurnForce)");
        g_origCannon = runtime.lookupFunction(kLcs.cannonUpdate);
        if (g_origCannon)
            runtime.replaceFunction(kLcs.cannonUpdate, &cannonUpdate);
        else
            game::logUnavailable("the water cannon's reach on the time step (lcs_physics_timing: no Update_OncePerFrame)");
    }
}
