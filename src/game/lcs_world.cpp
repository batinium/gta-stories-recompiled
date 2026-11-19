// [lcs] See lcs_world.h.
//
// Draw distance. As in VCS (game/vcs_world_detail.cpp, docs/game-internals.md 12d), CCamera::Process clamps
// LODDistMultiplier (TheCamera+0x170 in LCS) to 2, copies it into GenerationDistMultiplier (+0x174), then multiplies
// it by a constant 1.0 that nothing writes - the PS2 build's counterpart of ms_lodDistScale. Every model's LOD distance
// and the entity visibility test scale with it; spawn distances (the copy) do not. The setting writes that constant
// (draw / 100, 100..1000% on LCS, backed off by lcs_drawdist.cpp's memory / render-list governor) at every frame kick:
// 100% writes the executable's 1.0 back, the original game. The rest of a long draw distance (far plane and fog, the
// 300 m object scan, the level props' 100 m) is lcs_drawdist.cpp's.
// Keeping cars that are in view past the removal distance (PossiblyRemoveVehicle) is lcs_population.cpp's, with the
// density settings.
//
// LCS reads its constant once more than VCS: FUN_003465B0 (a FrontEndMenuManager reset, called from 0x346630) copies
// it into 0x3D9B34 as a plain 1.0. That function gets the executable's 1.0 for its read; the next kick puts the
// setting's value back.
//
// In the background: Pause. VCS holds its game thread at its frame kick (vcs::input::holdInBackground); the same call
// here, at LCS's kick, before the frame is sent.
//
// VCS_WORLD_DETAIL_HOOKS=0 leaves the draw distance alone, as for VCS.
#include "game/lcs_world.h"
#include "debug/profiler.h"

#include "game/game_table.h"
#include "game/lcs_drawdist.h"
#include "game/vcs_input.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "settings/settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lcs::world
{
    namespace
    {
        struct Addrs
        {
            uint32_t frameKick;  // the frame's display-list kick (lcs_hud.cpp hooks it too; chained)
            uint32_t lodScale;   // float 1.0: LODDistMultiplier's last factor in CCamera::Process
            uint32_t lodReader;  // fn: the other reader of lodScale (FrontEndMenuManager state reset)
        };
        constexpr Addrs kLcs{
            0x00212810u, // exact twin of VCS's 0x2723E8 (ratio 1.0, 33 instructions): the VIF1 display-list kick
            0x003D7578u, // CCamera::Process 0x28FAC8 at 0x291BF8..0x291C14: lwc1 0x170(s3); swc1 0x174(s3);
                         // lwc1 [0x3D7578]; mul.s; swc1 0x170(s3) - instruction for instruction VCS's 0x37B060..0x37B078
                         // with [0x4887F8] and +0x7DC / +0x7D0; the 2.0 clamp before it (lui 0x4000, c.olt, bc1tl swc1
                         // 0x170) as VCS's; the ELF's value 1.0. Two readers (VCS: one), no writer.
            0x003465B0u, // the second reader: lwc1 0x688(gp) = 0x3D7578 at 0x3465C4, stored to 0x3D9B34, then calls
                         // with FrontEndMenuManager 0x634610 (0x338130) and 0x634BC8 (0x343798)
        };

        constexpr float kOriginalScale = 1.0f;

        bool g_draw = false; // the draw distance is ours
        PS2Runtime::RecompiledFunction g_origKick = nullptr, g_origLodReader = nullptr;

        float rdf(const uint8_t *ram, uint32_t a)
        {
            float v;
            std::memcpy(&v, ram + (a & 0x01FFFFFFu), 4);
            return v;
        }
        void wrf(uint8_t *ram, uint32_t a, float f)
        {
            if (rdf(ram, a) != f)
                std::memcpy(ram + (a & 0x01FFFFFFu), &f, 4);
        }

        // the setting (100..1000% on LCS: lcs_drawdist.h), backed off by lcs_drawdist's memory / list governor
        float drawScale()
        {
            const float s = lcs::drawdist::lodScale();
            return s <= 1.0f ? kOriginalScale : s;
        }

        PS2_GUEST_CC void frameKick(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            vcs::profilerFrame();          // VCS_PROFILE_SPIKES (debug/profiler.h): LCS's frames too
            lcs::drawdist::onFrame(rdram); // the governor first: this frame's scale
            if (g_draw)
                wrf(rdram, kLcs.lodScale, drawScale());
            vcs::input::holdInBackground(runtime); // Game > In the background: Pause
            g_origKick(rdram, ctx, runtime);
        }

        PS2_GUEST_CC void lodReader(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            wrf(rdram, kLcs.lodScale, kOriginalScale); // its 1.0, whatever the draw distance
            g_origLodReader(rdram, ctx, runtime);
        }
    }

    void install(PS2Runtime &runtime)
    {
        const Addrs &A = kLcs;
        g_origKick = runtime.lookupFunction(A.frameKick); // lcs_hud's hook when it went in first: chained
        if (!g_origKick)
        {
            game::logUnavailable("draw distance / background pause (lcs_world: no frame kick)");
            return;
        }
        runtime.replaceFunction(A.frameKick, &frameKick);

        const char *env = std::getenv("VCS_WORLD_DETAIL_HOOKS");
        const float exe = rdf(runtime.memory().getRDRAM(), A.lodScale);
        if (env && env[0] == '0')
            std::fprintf(stderr, "[lcs-world] draw distance left alone (VCS_WORLD_DETAIL_HOOKS=0)\n");
        else if (exe != kOriginalScale) // another build, or the ELF not loaded: leave it alone
            std::fprintf(stderr, "[lcs-world] LOD scale 0x%x is %g, not 1: draw distance left alone\n", A.lodScale, exe);
        else if ((g_origLodReader = runtime.lookupFunction(A.lodReader)))
        {
            runtime.replaceFunction(A.lodReader, &lodReader);
            g_draw = true;
        }
        std::fprintf(stderr, "[lcs-world] %s, background pause\n", g_draw ? "draw distance" : "no draw distance");
    }
}
