// [lcs] See lcs_effects_timing.h. The effects of game/vcs_effects_timing.cpp that LCS has (twins by best.py and an
// instruction alignment, scratchpad mapra.py), with VCS's two tools: a PS2 frame "tick" (true on the frames where
// another 1/30 s has passed on CTimer's unscaled ms clock 0x3D72B8, which runs in slow motion and pauses too) for what
// the game re-rolls or steps once a frame, and the frame's share of a PS2 frame for what moves smoothly. Both are the
// game's own at 30 fps and below.
// 1. Lightning (CWeather::Update 0x324C50, VCS 0x3E3720 at 0.74): a burst starts on `(u16)rand() < 200` (ra 0x324EC0)
//    and ends on `(u8)rand() < 0x20` (ra 0x324E10), rolled every frame: the rolls count on ticks only. The thunder
//    (0x3DA1C0 = end + (20 - n) x 150 ms, n = min(frames, 20) at 0x3DA1CC; on 0x3DA1C4) gets the burst's PS2 frames.
// 2. Sun glare (ra 0x32549C, `*= 1 - (rand & 31) x 0.007` a frame): held between ticks.
// 3. Clouds (CClouds::Update 0x16B080, VCS's instruction for instruction): the rotation 0x3D9CB8 gets
//    sin(heading 0x43BDD0 - 0.85) x wind 0x3D9CB0 x 0.001 a frame and 0x3D9CBC a constant 18 a frame: scaled by the share.
// 4. Camera shake (CCamera::Process 0x28FAC8, rand at 0x291888): held between ticks.
// 5. The video-cam overlay (0x2464C8, Render2dStuff's; on 0x3D6740): its scanline offset 0x3D6754 flips once a call
//    and 200 noise dashes are re-rolled (ra 0x246750 / 0x24676C), as the second noise overlay (on 0x3D6744: ra
//    0x246820 / 0x24683C); the snapshot (on 0x3D674C) counts 0x3D6748 +1 a frame (white for 9, time stopped for 20):
//    flip and count undone, dashes held between ticks.
// 6. Peds fading in / out (CPed::ProcessControl 0x1D6F30: alpha from 0x2D7A38 read at ra 0x1D6FCC, -8 a frame while
//    ped +0x198 & 0x800000, else +16 to 255): moved back by that step between ticks.
// 7. Melee swing streaks (0x247E68 = VCS 0x169F88, 1.00): only on ticks.
// 8. Rubbish (CRubbish::Update 0x1F1C60, 0.97): visibility 0x3D9DC4 +-5 a frame (scaled); a sheet starts blowing on
//    `(frame & mask) == 0` with rand's pick (ra 0x1F21E0) of a sheet lying ready (state +0x1C == 1, 32 x 0x48 from
//    0x664478): between ticks it names one that is not.
// 9. Steam after rain (0x325758, from AddRain 0x325CA8): 800 frames counted down in 0x3D884C - put back between ticks.
// 10. Explosions (CExplosion::Update 0x163788, 48 x 0x50 from 0x4074B0; +0x34 both "active" and a step count, +1 a
//    frame, wrapping to 0 - the end - after 255: 1.8 s at 144 fps instead of 8.5): steps at 30 a second per explosion,
//    the molotov's / grenade's bursts (ra 0x164148 / 0x164344) only on fresh steps, `frame & 1` (coronas, light) on the
//    PS2 frames' parity until the next call of its caller 0x2A2618 (0x152898) starts.
// 11. Rain streaks (CWeather::RenderRainStreaks 0x326680, 0.89): each free slot starts a streak on `(rand & 0xF00) == 0`
//    (ra 0x326B28) every frame: the roll fails between ticks.
// Not in LCS: VCS's HUD bar flash (FUN_002AF6E0) and banknote spray (FUN_0032FD78) have no twin.
// rand (0x34FCE0, Park-Miller, seed 0x3D9178) is chained after lcs_population's hook; the listed return addresses get
// the same computation here. VCS_EFFECTS_TIMING=0 / Reduce Post Processing Effects leave all of it per frame;
// VCS_EFFECTS_LOG=1 prints what took effect every 30 s of real time.
#include "game/lcs_effects_timing.h"

#include "game/lcs_particle_timing.h"
#include "game/vcs_framerate.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace lcs::effects_timing
{
    namespace
    {
        constexpr uint32_t kRealClock = 0x003D72B8u;   // CTimer: whole ms of the ticks, unscaled, never paused
        constexpr uint32_t kGameClock = 0x003D9B70u;   // CTimer::m_snTimeInMilliseconds
        constexpr uint32_t kFrameCounter = 0x003D9B6Cu;
        constexpr uint32_t kTimeStep = 0x003D9B8Cu;
        constexpr float kPs2FrameMs = 1000.0f * 1001.0f / 30000.0f; // two NTSC fields

        constexpr uint32_t kRand = 0x0034FCE0u, kSeed = 0x003D9178u;
        constexpr uint32_t kWeatherUpdate = 0x00324C50u, kCloudsUpdate = 0x0016B080u, kRender2DFx = 0x002464C8u;
        constexpr uint32_t kGetAlpha = 0x002D7A38u, kStreakUpdate = 0x00247E68u, kRubbishUpdate = 0x001F1C60u;
        constexpr uint32_t kRainSteam = 0x00325758u;

        // rand's callers handled here (return addresses)
        constexpr uint32_t kLightningEnd = 0x00324E10u, kLightningStart = 0x00324EC0u, kSunGlare = 0x0032549Cu;
        constexpr uint32_t kCameraShake = 0x00291890u, kVideoDashX = 0x00246750u, kVideoDashY = 0x0024676Cu;
        constexpr uint32_t kNoiseDashX = 0x00246820u, kNoiseDashY = 0x0024683Cu;
        constexpr uint32_t kRubbishPick = 0x001F21E0u, kRainSpawn = 0x00326B28u;
        constexpr uint32_t kPedAlphaRead = 0x001D6FCCu;

        // weather / clouds / 2D / rubbish / steam data
        constexpr uint32_t kLightningOn = 0x003DA1C4u, kThunderAt = 0x003DA1C0u, kLightningFrames = 0x003DA1CCu;
        constexpr uint32_t kWind = 0x003D9CB0u, kCloudTurn = 0x003D9CB8u, kCloudSpin = 0x003D9CBCu, kHeading = 0x0043BDD0u;
        constexpr uint32_t kVideoCam = 0x003D6740u, kVideoLine = 0x003D6754u, kSnapshot = 0x003D674Cu,
                           kSnapshotCount = 0x003D6748u;
        constexpr uint32_t kRubbishFadingOut = 0x003D9DC0u, kRubbishVis = 0x003D9DC4u, kRubbishSheets = 0x00664478u;
        constexpr uint32_t kSteamCount = 0x003D884Cu;

        PS2Runtime::RecompiledFunction g_prevRand = nullptr, g_origWeather = nullptr, g_origClouds = nullptr,
                                       g_orig2DFx = nullptr, g_origStreak = nullptr, g_origRubbish = nullptr,
                                       g_origSteam = nullptr;

        bool envEnabled()
        {
            static const bool on = [] {
                const char *v = std::getenv("VCS_EFFECTS_TIMING");
                return !(v && v[0] == '0');
            }();
            return on;
        }
        bool enabled() { return envEnabled() && !lcs::particle_timing::reduced(); }
        bool logOn()
        {
            static const bool on = std::getenv("VCS_EFFECTS_LOG") != nullptr;
            return on;
        }

        template <typename T>
        T rd(const uint8_t *rdram, uint32_t addr)
        {
            T v;
            std::memcpy(&v, rdram + (addr & 0x1FFFFFFu), sizeof(T));
            return v;
        }
        template <typename T>
        void wr(uint8_t *rdram, uint32_t addr, T v)
        {
            std::memcpy(rdram + (addr & 0x1FFFFFFu), &v, sizeof(T));
        }

        // ---- the PS2 frame clock (vcs_effects_timing.cpp's) ----------------------------------------------------------
        struct Clock
        {
            uint32_t lastMs = 0;
            bool started = false;
            float carry = 0.0f;
            float share = 1.0f;
            bool tick = true;
            uint64_t seq = 0;
            uint64_t ticks = 0;
            float dt = kPs2FrameMs;
            float stepSum = 0.0f, tickStep = 0.0f;
        } g_clock;

        const Clock &clock(const uint8_t *rdram)
        {
            Clock &c = g_clock;
            const uint32_t now = rd<uint32_t>(rdram, kRealClock);
            if (c.started && now == c.lastMs)
                return c;
            const bool active = enabled() && (vcs::timing() == vcs::Timing::Independent || vcs::frameCap() != 30);
            const float dt = c.started ? static_cast<float>(now - c.lastMs) : kPs2FrameMs;
            c.started = true;
            c.lastMs = now;
            ++c.seq;
            if (!active || dt >= kPs2FrameMs * 0.9f || dt > 1000.0f)
            {
                c.carry = 0.0f;
                c.share = 1.0f;
                c.tick = true;
            }
            else
            {
                c.share = dt / kPs2FrameMs;
                c.carry += dt;
                c.tick = c.carry >= kPs2FrameMs * 0.9f;
                if (c.tick)
                    c.carry = std::clamp(c.carry - kPs2FrameMs, -0.1f * kPs2FrameMs, kPs2FrameMs);
            }
            c.ticks += c.tick ? 1u : 0u;
            c.dt = c.share < 1.0f ? dt : kPs2FrameMs;
            c.stepSum += rd<float>(rdram, kTimeStep);
            if (c.tick)
            {
                c.tickStep = c.stepSum;
                c.stepSum = 0.0f;
            }
            return c;
        }

        int dither(float amount, float &acc)
        {
            const float want = amount + acc;
            const float whole = std::trunc(want);
            acc = want - whole;
            return static_cast<int>(whole);
        }

        // ---- rand ----------------------------------------------------------------------------------------------------
        int32_t nextRand(uint8_t *rdram)
        {
            const int32_t seed = rd<int32_t>(rdram, kSeed);
            const int32_t hi = seed / 127773, lo = seed % 127773;
            int32_t v = static_cast<int32_t>(static_cast<uint32_t>(lo) * 16807u - static_cast<uint32_t>(hi) * 2836u);
            if (v <= 0)
                v = static_cast<int32_t>(static_cast<uint32_t>(v) + 0x7FFFFFFFu);
            wr<int32_t>(rdram, kSeed, v);
            return v;
        }

        struct Held
        {
            uint32_t ra;
            uint64_t seq = ~0ull;
            size_t next = 0;
            std::vector<int32_t> values;
        };
        Held g_held[] = {{kSunGlare}, {kCameraShake}, {kVideoDashX}, {kVideoDashY}, {kNoiseDashX}, {kNoiseDashY}};

        int32_t heldRand(uint8_t *rdram, Held &h, const Clock &c)
        {
            if (h.seq != c.seq)
            {
                h.seq = c.seq;
                h.next = 0;
                if (c.tick)
                    h.values.clear();
            }
            if (c.tick)
            {
                const int32_t v = nextRand(rdram);
                h.values.push_back(v);
                return v;
            }
            return h.next < h.values.size() ? h.values[h.next++] : nextRand(rdram);
        }

        enum Count
        {
            LightningRolls,
            GlareRolls,
            ShakeRolls,
            DashRolls,
            SnapshotSteps,
            VideoFlips,
            PedFadeSteps,
            StreakShifts,
            RubbishPicks,
            SteamSteps,
            RainRolls,
            CountN
        };
        const char *const kCountNames[CountN] = {"lightning rolls", "glare rolls", "shake rolls", "dash rolls",
                                                 "snapshot steps", "video flips", "ped fade steps", "streak shifts",
                                                 "rubbish picks", "steam steps", "rain streak rolls"};
        uint64_t g_counts[CountN] = {};

        struct LightningLog
        {
            uint32_t bursts = 0;
            double burstMs = 0.0;
        } g_lightLog;

        bool special(uint32_t ra)
        {
            switch (ra)
            {
            case kLightningEnd:
            case kLightningStart:
            case kSunGlare:
            case kCameraShake:
            case kVideoDashX:
            case kVideoDashY:
            case kNoiseDashX:
            case kNoiseDashY:
            case kRubbishPick:
            case kRainSpawn: return true;
            default: return false;
            }
        }

        PS2_GUEST_CC void rand(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (!special(ra) || !(enabled() || logOn()))
            {
                g_prevRand(rdram, ctx, runtime); // lcs_population's hook, then the game's
                return;
            }
            const Clock &c = clock(rdram);
            if (c.tick)
                ++g_counts[ra == kLightningStart || ra == kLightningEnd ? LightningRolls
                           : ra == kSunGlare                            ? GlareRolls
                           : ra == kCameraShake                         ? ShakeRolls
                           : ra == kRubbishPick                         ? RubbishPicks
                           : ra == kRainSpawn                           ? RainRolls
                                                                        : DashRolls];
            int32_t v;
            if (ra == kRainSpawn)
                v = c.tick ? nextRand(rdram) : 0xF00; // & 0xF00 != 0: no streak starts
            else if (ra == kLightningStart)
                v = c.tick ? nextRand(rdram) : 0xFFFF; // >= 200: no burst starts
            else if (ra == kLightningEnd)
                v = c.tick ? nextRand(rdram) : 0xFF; // >= 0x20: the burst goes on
            else if (ra == kRubbishPick)
            {
                v = nextRand(rdram);
                if (!c.tick) // a sheet that is not lying ready (state 1): none starts blowing
                    for (int32_t i = 0; i < 32; ++i)
                        if (rd<uint8_t>(rdram, kRubbishSheets + 0x1Cu + static_cast<uint32_t>(i) * 0x48u) != 1u)
                        {
                            v = (v & ~0x1F) | i;
                            break;
                        }
            }
            else
            {
                Held *h = std::find_if(std::begin(g_held), std::end(g_held), [ra](const Held &x) { return x.ra == ra; });
                v = heldRand(rdram, *h, c);
            }
            SET_GPR_S32(ctx, 2, v);
            ctx->pc = ra;
        }

        // ---- 1. lightning's thunder ------------------------------------------------------------------------------------
        struct Lightning
        {
            bool on = false;
            uint64_t startTicks = 0;
            uint32_t startMs = 0;
        } g_lightning;

        void logEffects(uint8_t *rdram);

        PS2_GUEST_CC void weatherUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (enabled() || logOn())
            {
                const Clock &c = clock(rdram);
                const bool on = rd<int32_t>(rdram, kLightningOn) != 0;
                Lightning &l = g_lightning;
                if (on && !l.on)
                {
                    l.startTicks = c.ticks - (c.tick ? 1u : 0u);
                    l.startMs = rd<uint32_t>(rdram, kRealClock);
                }
                else if (!on && l.on) // ended in the last Update: its length in PS2 frames, as the PS2 counted it
                {
                    const uint32_t thunder = rd<uint32_t>(rdram, kThunderAt);
                    const int32_t frames = rd<int32_t>(rdram, kLightningFrames);
                    const int32_t ps2 = static_cast<int32_t>(std::min<uint64_t>(c.ticks - (c.tick ? 1u : 0u) - l.startTicks, 20u));
                    if (enabled() && thunder && frames >= 0 && frames <= 20 && c.share < 1.0f)
                    {
                        const uint32_t endMs = thunder - static_cast<uint32_t>(20 - frames) * 150u;
                        wr<int32_t>(rdram, kLightningFrames, ps2);
                        wr<uint32_t>(rdram, kThunderAt, endMs + static_cast<uint32_t>(20 - ps2) * 150u);
                    }
                    ++g_lightLog.bursts;
                    g_lightLog.burstMs += rd<uint32_t>(rdram, kRealClock) - l.startMs;
                }
                l.on = on;
            }
            logEffects(rdram);
            g_origWeather(rdram, ctx, runtime);
        }

        // ---- 3. clouds -------------------------------------------------------------------------------------------------
        float g_cloudAcc = 0.0f;
        double g_cloudTurn = 0.0, g_cloudSpin = 0.0;

        PS2_GUEST_CC void cloudsUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const Clock &c = clock(rdram);
            const float wind = rd<float>(rdram, kWind), heading = rd<float>(rdram, kHeading);
            const float turn = std::sin(heading - 0.85f) * wind * 0.001f;
            const float step = rd<float>(rdram, kTimeStep);
            if (c.share < 1.0f)
            {
                wr<float>(rdram, kCloudTurn, rd<float>(rdram, kCloudTurn) - turn * (1.0f - c.share));
                wr<int32_t>(rdram, kCloudSpin, rd<int32_t>(rdram, kCloudSpin) - dither(18.0f * (1.0f - c.share), g_cloudAcc));
            }
            g_cloudTurn += turn * c.share;
            g_cloudSpin += (step * wind * 0.5f + 0.3f) * 60.0f - 18.0f * (1.0f - c.share);
            g_origClouds(rdram, ctx, runtime);
        }

        // ---- 5. video cam / snapshot -------------------------------------------------------------------------------------
        PS2_GUEST_CC void render2DFx(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const Clock &c = clock(rdram);
            if (c.tick)
            {
                g_counts[VideoFlips] += rd<int32_t>(rdram, kVideoCam) != 0;
                g_counts[SnapshotSteps] += rd<int32_t>(rdram, kSnapshot) != 0;
            }
            else
            {
                if (rd<int32_t>(rdram, kVideoCam) != 0) // the function flips the offset once in all
                    wr<uint8_t>(rdram, kVideoLine, rd<uint8_t>(rdram, kVideoLine) != 1u ? 1u : 0u);
                if (rd<int32_t>(rdram, kSnapshot) != 0)
                    wr<int32_t>(rdram, kSnapshotCount, rd<int32_t>(rdram, kSnapshotCount) - 1);
            }
            g_orig2DFx(rdram, ctx, runtime);
        }

        // ---- 6. peds fading ---------------------------------------------------------------------------------------------
        PS2_GUEST_CC void getAlpha(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            (void)runtime;
            const uint32_t ra = GPR_U32(ctx, 31);
            int32_t alpha = rd<int32_t>(rdram, GPR_U32(ctx, 4) + 0x14u); // 0x2D7A38: lw $v0, 0x14($a0)
            if (ra == kPedAlphaRead && (enabled() || logOn()))
            {
                const Clock &c = clock(rdram);
                const uint32_t ped = GPR_U32(ctx, 17); // $s1 (CPed::ProcessControl)
                const bool out = (rd<uint8_t>(rdram, ped + 0x19Au) & 0x80u) != 0; // ld 0x198 & 0x800000
                if (c.tick)
                    g_counts[PedFadeSteps] += out || alpha < 0xFF;
                else if (enabled())
                {
                    if (out) // -8 next (to 0)
                        alpha += 8;
                    else if (alpha < 0xFF) // +16 next (to 255)
                        alpha -= 16;
                }
            }
            SET_GPR_S32(ctx, 2, alpha);
            ctx->pc = ra;
        }

        // ---- 7. streaks ---------------------------------------------------------------------------------------------------
        PS2_GUEST_CC void streakUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!clock(rdram).tick)
            {
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            ++g_counts[StreakShifts];
            g_origStreak(rdram, ctx, runtime);
        }

        // ---- 8. rubbish ---------------------------------------------------------------------------------------------------
        float g_rubbishAcc = 0.0f;

        PS2_GUEST_CC void rubbishUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const Clock &c = clock(rdram);
            if (c.share < 1.0f)
            {
                const int back = dither(5.0f * (1.0f - c.share), g_rubbishAcc);
                const int32_t vis = rd<int32_t>(rdram, kRubbishVis);
                // fading in (0x3D9DC0 == 0): +5 next, to 255; out: -5, to 0
                wr<int32_t>(rdram, kRubbishVis, rd<int32_t>(rdram, kRubbishFadingOut) == 0 ? vis - back : vis + back);
            }
            g_origRubbish(rdram, ctx, runtime);
        }

        // ---- 9. steam after rain ------------------------------------------------------------------------------------------
        PS2_GUEST_CC void rainSteam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!clock(rdram).tick) // AddRain takes 1 off the count after this call
                wr<int32_t>(rdram, kSteamCount, rd<int32_t>(rdram, kSteamCount) + 1);
            else
                ++g_counts[SteamSteps];
            g_origSteam(rdram, ctx, runtime);
        }

        // ---- 10. explosions ----------------------------------------------------------------------------------------------
        constexpr uint32_t kExplosionUpdate = 0x00163788u, kExplosions = 0x004074B0u, kExplosionCount = 48u;
        constexpr uint32_t kExplosionNext = 0x00152898u; // the next call of 0x2A2618 after CExplosion::Update
        constexpr uint32_t kAddParticle = 0x001CD5B8u;
        constexpr uint32_t kBurstSites[] = {0x00164148u, 0x00164344u}; // type 1 (40 a step), types 10 / 11 (41)

        struct Explosion
        {
            bool active = false, fresh = true;
            uint8_t step = 0;
            float carry = 0.0f;
            uint32_t startMs = 0, bursts = 0;
        };
        Explosion g_explosions[kExplosionCount];
        struct ExplosionLog
        {
            uint32_t ended = 0;
            double ms = 0.0, steps = 0.0, bursts = 0.0;
        } g_explLog;
        bool g_counterSwapped = false;
        uint32_t g_realCounter = 0;
        PS2Runtime::RecompiledFunction g_origExplosions = nullptr, g_origNext = nullptr, g_origAddParticle = nullptr;

        PS2_GUEST_CC void explosionUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const Clock &c = clock(rdram);
            const bool on = enabled();
            if (on && (c.share < 1.0f || g_counterSwapped))
            {
                if (!g_counterSwapped)
                    g_realCounter = rd<uint32_t>(rdram, kFrameCounter);
                g_counterSwapped = true;
                wr<uint32_t>(rdram, kFrameCounter, (g_realCounter & ~1u) | static_cast<uint32_t>(c.ticks & 1u));
            }
            for (uint32_t i = 0; i < kExplosionCount; ++i)
            {
                const uint32_t rec = kExplosions + i * 0x50u;
                Explosion &e = g_explosions[i];
                const uint8_t b = rd<uint8_t>(rdram, rec + 0x34u);
                if (b == 0u)
                {
                    if (e.active)
                    {
                        ++g_explLog.ended;
                        g_explLog.ms += rd<uint32_t>(rdram, kRealClock) - e.startMs;
                        g_explLog.steps += e.step;
                        g_explLog.bursts += e.bursts;
                    }
                    e.active = false;
                    continue;
                }
                if (!e.active || rd<uint32_t>(rdram, rec + 0x3Cu) != 0u) // new, or in its delay
                {
                    e = {true, true, b, 0.0f, e.active ? e.startMs : rd<uint32_t>(rdram, kRealClock), e.active ? e.bursts : 0u};
                    continue;
                }
                if (!on)
                {
                    e.step = b;
                    e.fresh = true;
                    continue;
                }
                e.carry += c.dt;
                e.fresh = e.carry >= kPs2FrameMs * 0.9f;
                if (e.fresh)
                {
                    e.carry = std::clamp(e.carry - kPs2FrameMs, -0.1f * kPs2FrameMs, kPs2FrameMs);
                    e.step = static_cast<uint8_t>(std::min(e.step + 1, 255)); // the game's +1 after 255 ends it
                }
                wr<uint8_t>(rdram, rec + 0x34u, e.step);
            }
            g_origExplosions(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void explosionNext(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_counterSwapped)
            {
                wr<uint32_t>(rdram, kFrameCounter, g_realCounter);
                g_counterSwapped = false;
            }
            g_origNext(rdram, ctx, runtime);
        }

        bool explosionFresh(const uint8_t *rdram, uint32_t posAddr)
        {
            const float x = rd<float>(rdram, posAddr), y = rd<float>(rdram, posAddr + 4u);
            Explosion *best = nullptr;
            float bestD = 1e30f;
            for (uint32_t i = 0; i < kExplosionCount; ++i)
                if (g_explosions[i].active)
                {
                    const uint32_t rec = kExplosions + i * 0x50u;
                    const float dx = rd<float>(rdram, rec + 0x10u) - x, dy = rd<float>(rdram, rec + 0x14u) - y;
                    if (dx * dx + dy * dy < bestD)
                        bestD = dx * dx + dy * dy, best = &g_explosions[i];
                }
            if (best && best->fresh)
                ++best->bursts;
            return !best || best->fresh;
        }

        PS2_GUEST_CC void addParticleBurst(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (std::find(std::begin(kBurstSites), std::end(kBurstSites), ra) != std::end(kBurstSites) && enabled() &&
                !explosionFresh(rdram, GPR_U32(ctx, 5))) // a1: the particle's position
            {
                SET_GPR_U32(ctx, 2, 0u); // as with the pool full (these callers ignore it)
                ctx->pc = ra;
                return;
            }
            g_origAddParticle(rdram, ctx, runtime);
        }

        void logEffects(uint8_t *rdram)
        {
            if (!logOn())
                return;
            static uint32_t lastMs = 0;
            static uint64_t lastSeq = 0, lastTicks = 0;
            const Clock &c = clock(rdram);
            const uint32_t now = rd<uint32_t>(rdram, kRealClock);
            if (!lastMs)
            {
                lastMs = now, lastSeq = c.seq, lastTicks = c.ticks;
                return;
            }
            if (now - lastMs < 30000u)
                return;
            const double secs = (now - lastMs) / 1000.0;
            std::fprintf(stderr,
                         "[lcs-effects] %.0f s: %.1f frames/s, %.1f PS2 frames/s; lightning %u bursts (%.1f a minute, %.0f ms each); "
                         "clouds turn %.4f / s, spin %.0f / s\n",
                         secs, (c.seq - lastSeq) / secs, (c.ticks - lastTicks) / secs, g_lightLog.bursts, g_lightLog.bursts * 60.0 / secs,
                         g_lightLog.bursts ? g_lightLog.burstMs / g_lightLog.bursts : 0.0, g_cloudTurn / secs, g_cloudSpin / secs);
            std::fprintf(stderr, "[lcs-effects] taking effect, per second:");
            for (int i = 0; i < CountN; ++i)
                if (g_counts[i])
                    std::fprintf(stderr, " %s %.1f", kCountNames[i], g_counts[i] / secs);
            std::fprintf(stderr, "\n");
            std::memset(g_counts, 0, sizeof(g_counts));
            if (g_explLog.ended)
                std::fprintf(stderr, "[lcs-effects] explosions: %u ended, %.0f ms, %.1f steps, %.0f burst calls each\n", g_explLog.ended,
                             g_explLog.ms / g_explLog.ended, g_explLog.steps / g_explLog.ended, g_explLog.bursts / g_explLog.ended);
            g_explLog = {};
            g_lightLog = {};
            g_cloudTurn = g_cloudSpin = 0.0;
            lastMs = now, lastSeq = c.seq, lastTicks = c.ticks;
        }
    }

    void install(PS2Runtime &runtime)
    {
        struct Hook
        {
            uint32_t address;
            PS2Runtime::RecompiledFunction *original;
            PS2Runtime::RecompiledFunction replacement;
        };
        static PS2Runtime::RecompiledFunction noOriginal = nullptr;
        const Hook hooks[] = {
            {kRand, &g_prevRand, &rand}, // chains onto lcs_population's (installed before)
            {kGetAlpha, &noOriginal, &getAlpha},
            {kWeatherUpdate, &g_origWeather, &weatherUpdate},
            {kCloudsUpdate, &g_origClouds, &cloudsUpdate},
            {kRender2DFx, &g_orig2DFx, &render2DFx},
            {kStreakUpdate, &g_origStreak, &streakUpdate},
            {kRubbishUpdate, &g_origRubbish, &rubbishUpdate},
            {kRainSteam, &g_origSteam, &rainSteam},
            {kExplosionUpdate, &g_origExplosions, &explosionUpdate},
            {kExplosionNext, &g_origNext, &explosionNext},
            {kAddParticle, &g_origAddParticle, &addParticleBurst}, // chains onto lcs_particle_timing's
        };
        if (!envEnabled())
        {
            std::fprintf(stderr, "[lcs-effects-timing] off (VCS_EFFECTS_TIMING=0): lightning, clouds, shake ... once a frame\n");
            if (!logOn())
                return;
        }
        int n = 0;
        for (const Hook &h : hooks)
        {
            PS2Runtime::RecompiledFunction original = runtime.lookupFunction(h.address);
            if (!original)
            {
                std::fprintf(stderr, "[lcs-effects-timing] 0x%x not found: left per frame\n", h.address);
                continue;
            }
            *h.original = original;
            runtime.replaceFunction(h.address, h.replacement);
            ++n;
        }
        std::fprintf(stderr, "[lcs-effects-timing] %d hooks\n", n);
    }
}
