// [lcs] See lcs_world_timing.h. Things in the open world that LCS moves by a fixed amount each frame, never by the time
// step - right at the PS2's 30 fps, too fast above it (docs/LCS.md, high frame rates). Found by a survey of the frame
// counter's readers and of per-frame float accumulators (docs/game-internals.md, LCS timing survey), and measured with
// `VCS_TIMESTEP` + `lcstimestep` (one run, the same scene at two steps; tools/lcs_timing_audit.py).
//
// 1. The scrolling LED signs (GTA III's CScrollBar: 12 x 0x80 at 0x3DABE0, messages such as "SHARES DAV<15% ..." at
//    0x387A98 picked in Update). CMovingThings::Update 0x1013D0 calls CScrollBar::Update 0x101E00 for a bar that is
//    visible (+0x68, set by Update itself from the camera distance) or on `(frame + i) & 7 == 0`; Update counts +0 and on
//    every 8th call moves the text on by one letter (+0x24 against the length +0x28). So a visible sign scrolled 3.75
//    letters a second at 30 fps and 18 at 144. Here each bar runs Update only when a PS2 frame's worth of game time
//    has passed since its last run (eight PS2 frames for a bar the game calls because of the `& 7` stagger): credit in
//    PS2 frames from CTimer's ms clock (stands still while paused), at most one run a call.
//
// 2. The water's texture drift (CWaterLevel's render, 0x20DE28, called once a frame from 0x112418): four floats
//    gp-0x1998 / -0x1994 (+= d), -0x1990 (-= d + sin(t) x 0.0003) and -0x198C (+= d + cos(0.7 t) x 0.0003), with
//    d = wind (0x3D9E2C) x 0.0005 + 0.0006, wrapped to 0 / 1 - a frame's drift each frame, not times the step: the
//    water slid 4.8 times as fast at 144 fps. The renderer reads them right after the adds, so the hook corrects the
//    previous frame's add at the next call: what it added becomes add x (that frame's step / the PS2's). The values
//    drawn then advance by the scaled amount each frame (they sit one unscaled add ahead, a constant offset); a wrap is
//    left as the game did it.
//
// Only while the frame is not the PS2's (independent timing, or original timing at 60), as the other timing fixes.
// LCS_WORLD_TIMING=0 leaves all of it per frame; LCS_SCROLLBAR_TIMING=0 / LCS_WATER_TIMING=0 one item each.
// LCS_WORLD_TIMING_LOG=1 prints what took effect every 10 s of real time.
#include "game/lcs_world_timing.h"

#include "game/game_table.h"
#include "game/vcs_framerate.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lcs::world_timing
{
    namespace
    {
        constexpr uint32_t kTimeMs = 0x003D9B70u;   // CTimer::m_snTimeInMilliseconds (game_addrs timer.timeMs)
        constexpr uint32_t kTimeStep = 0x003D9B8Cu; // CTimer::ms_fTimeStep (game_addrs timer.timeStep)
        constexpr float kPs2Step = 50.0f / 30.0f;
        constexpr float kPs2FrameMs = 1000.0f / 30.0f;

        // 1. scroll bars
        constexpr uint32_t kScrollUpdate = 0x00101E00u; // CScrollBar::Update(a0 = bar): `+0 += 1; & 7` at 0x101ED8..EEC
        constexpr uint32_t kScrollBars = 0x003DABE0u;   // its caller 0x1013D0: lui 0x3E; addiu -0x5420; 12 x 0x80
        constexpr uint32_t kScrollBarSize = 0x80u, kScrollBarCount = 12u;
        // 2. water
        constexpr uint32_t kWaterRender = 0x0020DE28u; // adds at 0x20DF30..0x20DF94, wraps at 0x20DFA4..0x20E008
        constexpr uint32_t kWaterDrift[4] = {0x003D5558u, 0x003D555Cu, 0x003D5560u, 0x003D5564u}; // gp = 0x3D6EF0:
                                                       // -0x1998, -0x1994 (+), -0x1990 (-), -0x198C (+)
        constexpr bool kWaterDown[4] = {false, false, true, false};

        PS2Runtime::RecompiledFunction g_origScroll = nullptr, g_origWater = nullptr;

        template <typename T>
        T rd(const uint8_t *ram, uint32_t a)
        {
            T v;
            std::memcpy(&v, ram + (a & 0x01FFFFFFu), sizeof(T));
            return v;
        }
        template <typename T>
        void wr(uint8_t *ram, uint32_t a, T v)
        {
            std::memcpy(ram + (a & 0x01FFFFFFu), &v, sizeof(T));
        }

        bool envOn(const char *name)
        {
            const char *v = std::getenv(name);
            return !(v && v[0] == '0');
        }
        bool logOn()
        {
            static const bool on = std::getenv("LCS_WORLD_TIMING_LOG") != nullptr;
            return on;
        }
        bool active() { return vcs::timing() == vcs::Timing::Independent || vcs::frameCap() != 30; }

        struct Log
        {
            uint64_t scrollCalls = 0, scrollRuns = 0, waterFrames = 0;
            double waterDrift = 0.0; // what the first float moved after the correction
            std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
        } g_log;

        void maybeLog()
        {
            if (!logOn())
                return;
            const auto now = std::chrono::steady_clock::now();
            const double s = std::chrono::duration<double>(now - g_log.since).count();
            if (s < 10.0)
                return;
            std::fprintf(stderr,
                         "[lcs-world-timing] %.0f s: scroll bar updates %.1f/s run of %.1f/s called; water frames %.1f/s, drift %.4f/s\n",
                         s, g_log.scrollRuns / s, g_log.scrollCalls / s, g_log.waterFrames / s, g_log.waterDrift / s);
            g_log = {};
        }

        // ---- 1. scroll bars ---------------------------------------------------------------------------------------
        struct Bar
        {
            bool known = false;
            uint32_t lastMs = 0;
            float credit = 0.0f; // PS2 frames of game time not yet run
        };
        Bar g_bars[kScrollBarCount];

        PS2_GUEST_CC void scrollUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static const bool on = envOn("LCS_SCROLLBAR_TIMING");
            const uint32_t bar = GPR_U32(ctx, 4);
            const uint32_t n = (bar - kScrollBars) / kScrollBarSize;
            ++g_log.scrollCalls;
            if (on && bar >= kScrollBars && n < kScrollBarCount && (bar - kScrollBars) % kScrollBarSize == 0u)
            {
                Bar &b = g_bars[n];
                const uint32_t now = rd<uint32_t>(rdram, kTimeMs);
                // the game calls a visible bar every frame, a hidden one every eighth (CMovingThings::Update)
                const float need = rd<uint32_t>(rdram, bar + 0x68u) != 0u ? 1.0f : 8.0f;
                if (!active())
                    b.known = false;
                else
                {
                    if (!b.known || now < b.lastMs || now - b.lastMs > 2000u)
                        b = {true, now, need}; // first sight, a load or a long gap: run now
                    else
                        b.credit = std::min(b.credit + static_cast<float>(now - b.lastMs) / kPs2FrameMs, 2.0f * need);
                    b.lastMs = now;
                    if (b.credit < need * 0.9f)
                    {
                        ctx->pc = GPR_U32(ctx, 31); // not this frame: the bar as it is (Render reads its state)
                        maybeLog();
                        return;
                    }
                    b.credit -= need;
                }
            }
            ++g_log.scrollRuns;
            maybeLog();
            g_origScroll(rdram, ctx, runtime);
        }

        // ---- 2. water ---------------------------------------------------------------------------------------------
        struct Water
        {
            bool known = false;
            float value[4] = {};
            float scale = 1.0f; // the frame's step over the PS2's, of the call that left value[]
        } g_water;

        PS2_GUEST_CC void waterRender(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            static const bool on = envOn("LCS_WATER_TIMING");
            if (on && active())
            {
                Water &w = g_water;
                if (w.known)
                {
                    for (int i = 0; i < 4; ++i)
                    {
                        const float v = rd<float>(rdram, kWaterDrift[i]);
                        const float d = v - w.value[i];
                        const bool wrapped = kWaterDown[i] ? d > 0.0f : d < 0.0f;
                        if (d != 0.0f && !wrapped && w.scale < 1.0f)
                        {
                            const float corrected = w.value[i] + d * w.scale;
                            wr<float>(rdram, kWaterDrift[i], corrected);
                            if (i == 0)
                                g_log.waterDrift += d * w.scale, ++g_log.waterFrames;
                        }
                        else if (i == 0 && d != 0.0f && !wrapped)
                            g_log.waterDrift += d, ++g_log.waterFrames;
                    }
                }
                for (int i = 0; i < 4; ++i)
                    w.value[i] = rd<float>(rdram, kWaterDrift[i]);
                const float step = rd<float>(rdram, kTimeStep);
                w.scale = step >= 0.0f && step < kPs2Step ? step / kPs2Step : 1.0f;
                w.known = true;
            }
            else
                g_water.known = false;
            maybeLog();
            g_origWater(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (!envOn("LCS_WORLD_TIMING"))
        {
            std::fprintf(stderr, "[lcs-world-timing] off (LCS_WORLD_TIMING=0): scroll signs, water drift once a frame\n");
            return;
        }
        int n = 0;
        if ((g_origScroll = runtime.lookupFunction(kScrollUpdate)))
            runtime.replaceFunction(kScrollUpdate, &scrollUpdate), ++n;
        else
            game::logUnavailable("scroll signs at the PS2's rate (lcs_world_timing: no CScrollBar::Update)");
        if ((g_origWater = runtime.lookupFunction(kWaterRender)))
            runtime.replaceFunction(kWaterRender, &waterRender), ++n;
        else
            game::logUnavailable("water drift on the time step (lcs_world_timing: no water render)");
        std::fprintf(stderr, "[lcs-world-timing] %d hooks (scroll signs, water drift)\n", n);
    }
}
