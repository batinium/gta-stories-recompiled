// [lcs] See lcs_music_timing.h. The radio's progress while it isn't heard, as game/vcs_music_timing.cpp for VCS: the
// music manager keeps a 12-byte record per stream from +0x18 (length +0x18, stop position +0x1C, a count +0x20); a
// per-frame leaf adds 1 to the count of all 0x27 streams, and tuning back in starts a station at
// `stop + count * 33 + 500` modulo its length (FUN_00169FF0; VCS: count * 1500 + 100) - 33 ms a 1/30 s frame, so on
// the PS2 a station went on in real time while you were on foot; at 60 fps twice as fast. The hook adds the frame's
// step over the PS2's (50/30) to a running sum instead and hands the whole frames in it to the counts; longer frames
// than the PS2's keep 1. Only while the frame isn't the PS2's (independent timing, or original timing at 60);
// VCS_MUSIC_TIMESTEP=0 keeps the game's count.
#include "game/lcs_music_timing.h"

#include "game/game_table.h"
#include "game/vcs_framerate.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lcs::music_timing
{
    namespace
    {
        struct Addrs
        {
            uint32_t advanceCounts; // fn(manager): every stream's count += 1 (a leaf)
            uint32_t streams;       // its loop's count
            uint32_t timeStep;      // CTimer::ms_fTimeStep
        };
        constexpr Addrs kLcs{
            0x00169FB0u, // VCS 0x18B668 instruction for instruction (a3 = a0 + 0x20, mult by 0xC, lw / +1 / sw) but
                         // for `sltiu 0x27`; called from 0x1680E0 / 0x1684D0 (VCS's two services); the count read by
                         // FUN_00169FF0 (lw 0x20, x33, + 0x1C, + 500, divu by 0x18)
            0x27u,       // sltiu a0, a1, 0x27 at 0x169FD4
            0x003D9B8Cu, // game_addrs timer.timeStep
        };
        constexpr uint32_t kRecordCount = 0x20u, kRecordSize = 0x0Cu;
        constexpr float kPs2Step = 50.0f / 30.0f;

        double g_frames = 0.0; // PS2 frames not yet handed to the counts (one music manager)
        bool g_logged = false;

        PS2_GUEST_CC void advanceCounts(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t manager = GPR_U32(ctx, 4);
            uint32_t add = 1;
            if (vcs::timing() == vcs::Timing::Independent || vcs::frameCap() != 30)
            {
                float step;
                std::memcpy(&step, rdram + kLcs.timeStep, sizeof(step));
                const double k = std::isfinite(step) ? std::clamp(static_cast<double>(step) / kPs2Step, 0.0, 1.0) : 1.0;
                g_frames += k;
                add = static_cast<uint32_t>(g_frames);
                g_frames -= add;
                if (!g_logged)
                {
                    g_logged = true;
                    std::fprintf(stderr, "[lcs-music] stations off the air advance on the time step (step %.3f, x%.3f)\n",
                                 step, k);
                }
            }
            for (uint32_t i = 0; i < kLcs.streams; ++i)
            {
                uint8_t *count = rdram + ((manager + kRecordCount + i * kRecordSize) & 0x01FFFFFFu);
                uint32_t v;
                std::memcpy(&v, count, sizeof(v));
                v += add;
                std::memcpy(count, &v, sizeof(v));
            }
            ctx->pc = GPR_U32(ctx, 31);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *v = std::getenv("VCS_MUSIC_TIMESTEP"); v && v[0] == '0')
            return;
        if (!runtime.lookupFunction(kLcs.advanceCounts))
        {
            game::logUnavailable("the radio off the air on the time step (lcs_music_timing)");
            return;
        }
        runtime.replaceFunction(kLcs.advanceCounts, &advanceCounts);
    }
}
