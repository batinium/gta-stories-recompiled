// [lcs] See lcs_framerate.h. LCS paces itself exactly as VCS does (docs/game-internals.md 4), one engine generation
// earlier:
// - the limiter FUN_002B5400 (VCS 0x3179A0, same body: wait for the previous list, kick the next, `do WaitVBlank();
//   while (graphics+4 < 2)`, reset, flip 0x2D9640) - 30 fps on the PS2, two VBlanks a frame;
// - the VBlank counter FUN_002B5528 (VCS 0x317AC8: +4 += 1), registered through 0x2B5508 for the graphics object
//   0x43EA70; WaitVBlank FUN_002D9360 (VCS 0x3703B8: spins on a flag, clears it);
// - CTimer::Update FUN_002B4CF8 (VCS 0x2F4960): ticks from a kernel timer FUN_00368ED8 (VCS 0x451C00; only caller
//   Update), ms clocks += (int)(ticks * scale / 147456) truncated per frame, step = ticks * scale * 50 / 147456000,
//   at least 0.5, at most 3.0, the ms clock at most +60 a frame, m_FrameCounter += 1.
// So the hooks are VCS's: in independent timing the ticks are the host clock in whole milliseconds (the truncation's
// 1.5-5% slow clock fixed), the limiter's wait (ra 0x2B54B0) presents, paces to the cap and brings the emulated clock
// up to real time (vcs::framerate_seam::limiterWait), the counter adds 2 a VBlank in original timing with a cap above
// 30 (60 fps); the 2D pass (Render2dStuff 0x1F6338) sees a frame counter at 30 a second (the HUD's flashing).
//
// The minimum step. VCS lifts CTimer's 0.5 floor (1/100 s) on Update's second timer check; LCS's Update checks the
// timer once, before the clamp, so there is no hook point after it. In independent timing Update itself runs on the
// host instead (hostTimerUpdate: the same reads, products, truncations and clamps as 0x2B4CF8, in its order), with the
// floor lowered to 0.2 (4 ms, kMinStep): the cap's ceiling is 240 fps, as VCS's. The code that misbehaved below the old
// floor is patched in game/lcs/overrides.toml (ped movement 0x1D5978 and the others listed there). Every Update still
// gets at least the floor's whole milliseconds (hostTicks), so the steps add up to real time.
// LCS_HOST_TIMER=0: the game's Update and its 0.5 floor (ceiling 100 fps, 10 ms per Update), as before;
// LCS_MIN_STEP=<x> another floor for experiments (the ceiling follows: 1000 / its whole ms, at most 240).
// Measured (VCS_TIMESTEP 30 - 240): run 5.28 m/s per game second at all; without the ped patch 0.47-0.55 at 240.
//
// VCS_FRAMERATE_HOOKS=0 installs none of this (the game's own limiter and CTimer, as before).
#include "game/lcs_framerate.h"

#include "game/game_table.h"
#include "game/vcs_framerate.h"
#include "render/presenter.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_control.h"
#include "runtime/ps2_host_std.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lcs::framerate
{
    namespace
    {
        struct Addrs
        {
            uint32_t countVBlank;  // fn: graphics object (a0) +4 += 1, from a VBlank callback
            uint32_t timerTicks;   // fn: a kernel timer's 64-bit count (a0 = timer id), read by CTimer::Update only
            uint32_t waitVBlank;   // fn
            uint32_t limiterWait;  // return address of the limiter's VBlank wait
            uint32_t timerUpdate;  // fn: CTimer::Update
            uint32_t twoDPass;     // fn: the frame's 2D (Render2dStuff)
            uint32_t prevTicks;    // u64: CTimer's tick count of the last Update
            uint32_t frameCounter; // CTimer::m_FrameCounter
            uint32_t timeMs;       // CTimer::m_snTimeInMilliseconds
            // hostTimerUpdate: the rest of what Update reads and writes
            uint32_t timerId;      // s32: the kernel timer Update reads (< 0: not made yet - the game's Update makes it)
            uint32_t prevTimeMs;   // m_snPreviousTimeInMilliseconds
            uint32_t sysMs;        // a ms clock += whole ms of the ticks (unscaled, never paused)
            uint32_t frameTicks;   // this frame's ticks (0 when paused)
            uint32_t pausedA;      // three paused flags: no ticks, no floor
            uint32_t pausedB;
            uint32_t pausedC;      // u8
            uint32_t noFloor;      // != 0: no floor
            uint32_t timeScale;    // ms_fTimeScale
            uint32_t timeMs2;      // a second ms clock += the same scaled ms
            uint32_t timeStep;     // ms_fTimeStep (1.0 = 1/50 s)
            uint32_t stepNonClip;  // ms_fTimeStepNonClipped (the cutscene clock sums it)
            uint32_t noMaxMode;    // s16: == 2: no 3.0 / +60 ms clamp
            uint32_t fixedStep;    // u8: == 1: step 1.0, ms +16
        };
        constexpr Addrs kLcs{
            0x002B5528u, // lw v0, 4(a0); addiu 1; sw; jr - only caller 0x2B5508 (a0 = 0x43EA70), whose address
                         // the limiter loads (0x2B5400); VCS 0x317AC8 / 0x317AA8
            0x00368ED8u, // called once, at Update's top (0x2B4D0C, a0 = gp+0x3D0 the timer id); returns the 64-bit
                         // count Update stores at gp+0x3E0 and subtracts its last one from (VCS 0x451C00)
            0x002D9360u, // lw [0x3D9A44]; beqz back; sw zero: VCS 0x3703B8's spin
            0x002B54B0u, // 0x2B54A8 jal WaitVBlank; 0x2B54B0 lw v0, 4(s2); sltiu 2; bnez back (VCS 0x317A48..)
            0x002B4CF8u, // game_addrs timer.update (two 0x36E38E39 scalings, one 0x34B60B61, the 0.5 / 3.0 / 60 clamps)
            0x001F6338u, // Render2dStuff: letterbox, scope bars, CHud::Draw 0x23F438, messages, CFont::DrawFonts
                         // (lcs_hud.cpp); VCS 0x21F348
            0x003D72D0u, // gp+0x3E0: `ld a0; sd v0; dsubu v0, v0, a0` at 0x2B4D14..0x2B4D24
            0x003D9B6Cu, // game_addrs timer.frameCounter (+= 1 at Update's end)
            0x003D9B70u, // game_addrs timer.timeMs
            // from Update's body (0x2B4CF8.., gp = 0x3D6EF0):
            0x003D72C0u, // gp+0x3D0: `lw a0, 0x3D0(gp)` before the timer read; the ensure 0x2B4B88 tests it `bgez`
            0x003D9BC4u, // gp+0x2CD4 = the time ms at the top (0x2B4D10); base of the +60 / +16 clamps (0x2B4F38)
            0x003D72B8u, // gp+0x3C8 += ticks / 0x24000 (divu, 0x2B4D30..0x2B4D5C)
            0x003DA0D8u, // -0x5F28 = gp+0x31E8: the tick difference, zeroed when paused (0x2B4D84 / 0x2B4D6C)
            0x003D9CB4u, // -0x634C = gp+0x2DC4: bnez -> ticks 0 (0x2B4D58), no floor (0x2B4EE0)
            0x003D9C68u, // -0x6398: the same (0x2B4D68, 0x2B4EF0)
            0x003D8BA4u, // -0x745C lbu: the same (0x2B4D78, 0x2B4F00)
            0x003D674Cu, // `lui 0x3D; lw 0x674C`: beql zero -> the 0.5 stored (0x2B4F10)
            0x003D9CE4u, // -0x631C = gp+0x2DF4: the scale in all three products
            0x003DA0D4u, // -0x5F2C: += the second product's whole ms (0x2B4E54..0x2B4E68, gp+0x31E4)
            0x003D9B8Cu, // gp+0x2C9C = -0x6474: stored before the ensure call, clamped after (game_addrs timer.timeStep)
            0x003D9D9Cu, // gp+0x2EAC: the step after the floor, before the 3.0 (0x2B4F30)
            0x003D9CECu, // -0x6314 lh, compared with 2 (0x2B4F28)
            0x003D9C8Cu, // -0x6374 lbu, compared with 1 (0x2B4F70)
        };

        // the step floor (see the top): 0.2 = 4 ms with the host Update, else the game's 0.5 = 10 ms
        constexpr double kMaxFps = 240.0, kMaxFpsGameFloor = 100.0;
        constexpr uint64_t kBusHz = 147456000ull; // CTimer: ms = ticks / 147456
        constexpr uint64_t kFloorMsGame = 10;
        constexpr float kMinStep = 0.2f;     // 4 ms x 50 / 1000
        constexpr uint64_t kMaxDebtMs = 100; // hostTicks

        PS2Runtime::RecompiledFunction g_origTicks = nullptr, g_origWait = nullptr, g_origTwoD = nullptr,
                                       g_origUpdate = nullptr;
        bool g_counterSwapped = false; // the 2D pass runs on the 30 Hz counter (game thread)
        uint32_t g_realCounter = 0;

        bool independent() { return vcs::timing() == vcs::Timing::Independent; }

        // LCS_MIN_STEP=<x> (experiments): another floor for the host Update, in steps (0.05 = 1 ms)
        float minStep()
        {
            static const float step = [] {
                const char *v = std::getenv("LCS_MIN_STEP");
                const float x = v && *v ? static_cast<float>(std::atof(v)) : 0.0f;
                return x > 0.0f && x <= 0.5f ? x : kMinStep;
            }();
            return step;
        }
        uint64_t floorMsHost() { return static_cast<uint64_t>(std::ceil(minStep() * 20.0f - 1e-4f)); }

        bool hostUpdate() // LCS_HOST_TIMER=0: the game's Update (and its floor)
        {
            static const bool on = [] {
                const char *v = std::getenv("LCS_HOST_TIMER");
                return !(v && v[0] == '0');
            }();
            return on && g_origUpdate;
        }

        uint64_t hostNs()
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(ps2x::hostNow().time_since_epoch()).count());
        }

        // VCS_TIMESTEP=<N>: exactly 1/N s a frame, unpaced (vcs_framerate.cpp). In such a run the control command
        // `lcstimestep <N>` changes N (timing surveys: the same scene at two steps, tools/lcs_timing_audit.py).
        std::atomic<uint32_t> g_forcedFps{0};
        uint32_t forcedStepFps()
        {
            static const uint32_t fps = [] {
                const char *v = std::getenv("VCS_TIMESTEP");
                const int n = v ? std::atoi(v) : 0;
                return static_cast<uint32_t>(n > 0 ? n : 0);
            }();
            if (!fps)
                return 0;
            const uint32_t now = g_forcedFps.load(std::memory_order_relaxed);
            return now ? now : fps;
        }

        uint32_t rd32(const uint8_t *ram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, ram + (a & 0x01FFFFFFu), 4);
            return v;
        }
        void wr32(uint8_t *ram, uint32_t a, uint32_t v) { std::memcpy(ram + (a & 0x01FFFFFFu), &v, 4); }
        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & 0x01FFFFFFu), 4);
            return v;
        }
        void wrf(uint8_t *ram, uint32_t a, float v) { std::memcpy(ram + (a & 0x01FFFFFFu), &v, 4); }
        float bitsF(uint32_t bits)
        {
            float f;
            std::memcpy(&f, &bits, 4);
            return f;
        }

        // the timer's count in independent timing: the host clock, continuing CTimer's last count, in whole
        // milliseconds (vcs_framerate.cpp timerTicks)
        bool g_rebaseHost = false; // noteTimingMode: independent timing switched on again while playing
        uint64_t hostTicks(const uint8_t *rdram)
        {
            static bool started = false;
            static uint64_t base = 0, lastHost = 0, virtualNs = 0, forcedTicks = 0, forcedRest = 0, lastMs = 0;
            if (!started || g_rebaseHost)
            {
                std::memcpy(&base, rdram + kLcs.prevTicks, 8);
                lastHost = hostNs();
                virtualNs = forcedTicks = forcedRest = lastMs = 0;
                started = true;
                g_rebaseHost = false;
            }
            uint64_t elapsed;
            if (const uint32_t fps = forcedStepFps())
            {
                // kBusHz / fps a call, the remainder carried (the step may change mid-run)
                forcedRest += kBusHz;
                forcedTicks += forcedRest / fps;
                forcedRest %= fps;
                elapsed = forcedTicks;
            }
            else
            {
                const uint64_t now = hostNs();
                const float speed = vcs::fastForward();
                virtualNs += speed == 1.0f ? now - lastHost : static_cast<uint64_t>(static_cast<double>(now - lastHost) * speed);
                lastHost = now;
                elapsed = (virtualNs / 1000000000ull) * kBusHz + (virtualNs % 1000000000ull) * kBusHz / 1000000000ull;
            }
            constexpr uint64_t kTicksPerMs = kBusHz / 1000u;
            // The step floor (10 ms, or 4 with the host Update): a frame shorter than it got the floor's step anyway
            // but only its own milliseconds on the clock, so everything that sums the steps - the cutscene clock among
            // it - ran ahead of the game clock: 1.4% at a cap of 100, where the host's frames jitter around 10 ms. Every
            // Update gets at least the floor here; the time borrowed comes off the next frames, so the steps add up to
            // real time. The debt is kept under kMaxDebtMs (loading screens call Update in tight loops): past it a
            // short frame is left to the clamp, as before.
            const uint64_t floorMs = hostUpdate() ? floorMsHost() : kFloorMsGame;
            const uint64_t realMs = elapsed / kTicksPerMs;
            uint64_t ms = realMs;
            if (realMs < lastMs + floorMs)
                ms = lastMs + floorMs - realMs <= kMaxDebtMs ? lastMs + floorMs : std::max(lastMs, realMs);
            present::paceNote('S', static_cast<double>(ms - std::min(ms, lastMs)));
            lastMs = ms;
            return base + ms * kTicksPerMs;
        }

        // [gta-recomp] The timing switched while playing (live since 2026-11-18; vcs_framerate.cpp timerTicks): each switch
        // continues from CTimer's last count - independent timing's host clock starts again, original timing (once
        // switched to) counts the emulated bus clock (half the EE's cycles) from it instead of the kernel timer, and the
        // guest work charge is the original's again. Called where Update starts (timerUpdate) and in the timer read.
        bool g_switched = false;
        uint64_t g_emuBase = 0, g_emuCycle0 = 0;
        void noteTimingMode(uint8_t *rdram, PS2Runtime *runtime)
        {
            static int lastMode = -1;
            const int mode = independent() ? 1 : 0;
            if (lastMode >= 0 && lastMode != mode)
            {
                g_switched = true;
                if (mode)
                    g_rebaseHost = true;
                else
                {
                    std::memcpy(&g_emuBase, rdram + kLcs.prevTicks, 8);
                    g_emuCycle0 = runtime->eeScheduler().fastClock().cycle;
                    runtime->eeScheduler().setGuestChargeShift(0);
                }
                std::fprintf(stderr, "[lcs-framerate] timing switched to %s while playing\n", mode ? "independent" : "original");
            }
            lastMode = mode;
        }

        PS2_GUEST_CC void timerTicks(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            noteTimingMode(rdram, runtime);
            if (!independent())
            {
                if (!g_switched)
                {
                    g_origTicks(rdram, ctx, runtime);
                    return;
                }
                SET_GPR_U64(ctx, 2, g_emuBase + (runtime->eeScheduler().fastClock().cycle - g_emuCycle0) / 2u);
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            SET_GPR_U64(ctx, 2, hostTicks(rdram));
            ctx->pc = GPR_U32(ctx, 31);
        }

        PS2_GUEST_CC void waitVBlank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (independent() && GPR_U32(ctx, 31) == kLcs.limiterWait)
            {
                vcs::framerate_seam::limiterWait(rdram, ctx, runtime); // $s2 = the graphics object, as in VCS
                return;
            }
            g_origWait(rdram, ctx, runtime);
        }

        // Render2dStuff: the HUD's flashing counts frames (GTA3's CHud: m_FrameCounter & 8 ...): above 30 fps it sees
        // a counter whose low bits advance 30 times a game second; the real one is back as Update starts
        PS2_GUEST_CC void twoDPass(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const bool fast = independent() ? vcs::framerate_seam::effectiveFps() != 30.0 : vcs::frameCap() != 30;
            if (fast && !g_counterSwapped)
            {
                g_realCounter = rd32(rdram, kLcs.frameCounter);
                const uint32_t at30 = static_cast<uint32_t>(static_cast<uint64_t>(rd32(rdram, kLcs.timeMs)) * 3u / 100u);
                wr32(rdram, kLcs.frameCounter, (g_realCounter & ~0x3Fu) | (at30 & 0x3Fu));
                g_counterSwapped = true;
            }
            { // LCS_HUD_BLINK_LOG=1: how often the HUD's flash bits (counter & 8, & 4) flip per real second, every 5 s
                static const bool log = std::getenv("LCS_HUD_BLINK_LOG") != nullptr;
                static uint64_t since = 0, passes = 0, flips8 = 0, flips4 = 0;
                static uint32_t last = 0, sinceMs = 0;
                if (log)
                {
                    const uint32_t seen = rd32(rdram, kLcs.frameCounter);
                    const uint64_t now = hostNs();
                    if (!since)
                        since = now, sinceMs = rd32(rdram, kLcs.timeMs);
                    else
                    {
                        flips8 += ((seen ^ last) & 8u) != 0;
                        flips4 += ((seen ^ last) & 4u) != 0;
                        ++passes;
                    }
                    last = seen;
                    if (now - since >= 5000000000ull)
                    {
                        const double s = (now - since) / 1e9;
                        const double gs = (rd32(rdram, kLcs.timeMs) - sinceMs) / 1000.0; // game seconds (VCS_TIMESTEP)
                        std::fprintf(stderr, "[lcs-framerate] HUD flash: %.1f 2D passes/s, bit 8 flips %.2f/s, bit 4 %.2f/s; "
                                     "per game second %.2f / %.2f\n", passes / s, flips8 / s, flips4 / s,
                                     gs > 0 ? flips8 / gs : 0.0, gs > 0 ? flips4 / gs : 0.0);
                        since = now, sinceMs = rd32(rdram, kLcs.timeMs);
                        passes = flips8 = flips4 = 0;
                    }
                }
            }
            g_origTwoD(rdram, ctx, runtime);
        }

        // CTimer::Update 0x2B4CF8 on the host (independent timing, the timer made): the game's, with the floor at
        // kMinStep. Products in the game's order on the PS2's float rules (ps2_fmul), conversions as cvt.s.w / cvt.w.s.
        void hostTimerUpdate(uint8_t *rdram)
        {
            const Addrs &A = kLcs;
            wr32(rdram, A.prevTimeMs, rd32(rdram, A.timeMs));
            const uint64_t ticks = hostTicks(rdram);
            uint64_t prev;
            std::memcpy(&prev, rdram + A.prevTicks, 8);
            std::memcpy(rdram + A.prevTicks, &ticks, 8);
            uint32_t d = static_cast<uint32_t>(ticks - prev); // dsll32 / dsra32: the low word
            wr32(rdram, A.frameTicks, d);
            wr32(rdram, A.sysMs, rd32(rdram, A.sysMs) + d / 0x24000u);
            const bool paused = rd32(rdram, A.pausedA) != 0 || rd32(rdram, A.pausedB) != 0 || rdram[A.pausedC] != 0;
            if (paused)
            {
                d = 0;
                wr32(rdram, A.frameTicks, 0);
            }
            // cvt.s.w, or for a count with the top bit set ((d >> 1) | (d & 1)) x 2
            const float t = static_cast<int32_t>(d) >= 0 ? static_cast<float>(static_cast<int32_t>(d))
                                                          : 2.0f * static_cast<float>(static_cast<int32_t>((d >> 1) | (d & 1u)));
            const float scale = rdf(rdram, A.timeScale);
            const float perMs = bitsF(0x36E38E39u);   // 1 / 147456
            const float perStep = bitsF(0x34B60B61u); // 50 / 147456000
            const uint32_t ms = static_cast<uint32_t>(ps2FpuCvtWS(ps2_fmul(ps2_fmul(scale, t), perMs)));
            wr32(rdram, A.timeMs, rd32(rdram, A.timeMs) + ms);
            wr32(rdram, A.timeMs2, rd32(rdram, A.timeMs2) + ms);
            float step = ps2_fmul(ps2_fmul(t, scale), perStep);
            wrf(rdram, A.timeStep, step);
            // (the timer ensure 0x2B4B88 does nothing once the timer is made)
            if (step < 0.5f && !paused && rd32(rdram, A.noFloor) == 0)
            {
                step = std::max(step, minStep()); // the game: 0.5
                wrf(rdram, A.timeStep, step);
            }
            wrf(rdram, A.stepNonClip, step);
            if (static_cast<int16_t>(rd32(rdram, A.noMaxMode) & 0xFFFFu) != 2)
            {
                const uint32_t most = rd32(rdram, A.prevTimeMs) + 60u, now = rd32(rdram, A.timeMs);
                wrf(rdram, A.timeStep, std::min(3.0f, step));
                wr32(rdram, A.timeMs, now < most ? now : most);
            }
            if (rdram[A.fixedStep] == 1)
            {
                wrf(rdram, A.timeStep, 1.0f);
                wr32(rdram, A.timeMs, rd32(rdram, A.prevTimeMs) + 16u);
            }
            wr32(rdram, A.frameCounter, rd32(rdram, A.frameCounter) + 1u);
        }

        PS2_GUEST_CC void timerUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_counterSwapped)
            {
                wr32(rdram, kLcs.frameCounter, g_realCounter);
                g_counterSwapped = false;
            }
            noteTimingMode(rdram, runtime); // [gta-recomp] a switch while playing (the host Update reads no timer)
            if (independent() && hostUpdate() && static_cast<int32_t>(rd32(rdram, kLcs.timerId)) >= 0)
            {
                hostTimerUpdate(rdram);
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
            g_origUpdate(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        const Addrs &A = kLcs;
        if (const char *v = std::getenv("VCS_FRAMERATE_HOOKS"); v && v[0] == '0')
        {
            std::fprintf(stderr, "[lcs-framerate] no hooks (VCS_FRAMERATE_HOOKS=0): the game's own limiter\n");
            return;
        }
        g_origTicks = runtime.lookupFunction(A.timerTicks);
        g_origWait = runtime.lookupFunction(A.waitVBlank);
        g_origTwoD = runtime.lookupFunction(A.twoDPass);
        g_origUpdate = runtime.lookupFunction(A.timerUpdate);
        if (!g_origTwoD)
            g_origUpdate = nullptr; // the two go together (the HUD counter)
        vcs::framerate_seam::setMaxFps(hostUpdate() ? std::min(kMaxFps, 1000.0 / floorMsHost()) : kMaxFpsGameFloor);
        runtime.replaceFunction(A.countVBlank, &vcs::framerate_seam::countVBlank);
        if (g_origTwoD && g_origUpdate)
        {
            runtime.replaceFunction(A.twoDPass, &twoDPass);
            runtime.replaceFunction(A.timerUpdate, &timerUpdate);
        }
        if (!g_origTicks || !g_origWait)
        {
            game::logUnavailable("frame-rate independent timing (lcs_framerate: no CTimer / WaitVBlank)");
            vcs::setTiming(vcs::Timing::Original);
            return;
        }
        runtime.replaceFunction(A.timerTicks, &timerTicks);
        runtime.replaceFunction(A.waitVBlank, &waitVBlank);
        // lcstimestep <N> -> ok <N>: another fixed step in a VCS_TIMESTEP run (refused without one)
        ps2control::addCommand("lcstimestep", [](const std::string &args, ps2control::Reply reply) {
            const int n = std::atoi(args.c_str());
            if (!forcedStepFps() || n < 5 || n > 1000)
                return reply("error usage: lcstimestep <5..1000> (only in a VCS_TIMESTEP run)");
            g_forcedFps.store(static_cast<uint32_t>(n));
            reply("ok " + std::to_string(n));
        });
        if (hostUpdate())
            std::fprintf(stderr, "[lcs-framerate] limiter, CTimer (host Update, step floor %.3f), HUD counter (cap at most %.0f fps)\n",
                         minStep(), std::min(kMaxFps, 1000.0 / floorMsHost()));
        else
            std::fprintf(stderr, "[lcs-framerate] limiter, CTimer, HUD counter (cap at most %.0f fps)\n", kMaxFpsGameFloor);
    }
}
