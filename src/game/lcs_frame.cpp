// [lcs] Liberty City Stories' game-frame boundary for the runtime's game-independent per-frame users (VCS: the frame
// kick hook markFrameKick in vcs_overrides.cpp, which LCS never installs):
//  - the perf overlay's frame mark (runtime/ps2_perf.h): without it the overlay falls back to VBlanks and shows 60 fps
//    whatever the game runs at;
//  - the automation rig (debug/rig.h): its frame count (gamectl `frames`, Display > Show frame rate, the Android stats
//    overlay's counter: all 0 on LCS before) and its game-thread jobs (`watch`, `pause`; they never ran on LCS).
// The frame kick 0x212810 (the twin of VCS's 0x2723E8: kicks the frame's VIF1 display list once per rendered frame) is
// chained: whatever feature hooked it first (lcs_hud) still runs after this.
#include "game/lcs_frame.h"

#include "debug/rig.h"
#include "game/dualworld.h"
#include "game/lcs_crossover.h"
#include "game/vcs_mods.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_perf.h"

#include <cstdio>

namespace lcs::frame
{
    namespace
    {
        constexpr uint32_t kFrameKick = 0x00212810u;
        PS2Runtime::RecompiledFunction g_next = nullptr;

        PS2_GUEST_CC void frameKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            ps2perf::markGameFrame();
            vcs::rig::onGameFrame(rdram, ctx, runtime);
            vcs::mods::onGameFrame(rdram); // the mod loader's device tests and load-state log (game/vcs_mods.h)
            lcs::crossover::onFrame(rdram, ctx, runtime); // [crossover] VCS vehicles' pool slots back to stock once unloaded
            dualworld::onGameFrame(rdram); // [dualworld] the camera / clock exchange (game/dualworld.h)
            g_next(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_next = runtime.lookupFunction(kFrameKick);
        if (!g_next)
        {
            std::fprintf(stderr, "[lcs] frame kick 0x%x not in the function map: no frame mark\n", kFrameKick);
            return;
        }
        runtime.replaceFunction(kFrameKick, &frameKick);
    }
}
