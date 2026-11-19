// [lcs] Long draw distance and pop-in for Liberty City Stories (docs/game-internals.md 12e, docs/LCS.md).
//
// lcs_world.cpp writes the LOD distance scale (Enhancements > World > Draw distance); this module does the rest a long
// draw distance needs and what makes objects appear late:
// - the far plane and fog follow the draw distance (Enhancements > World > Fog and far plane),
// - the entity scan (CRenderer::ScanWorld) reaches 300 m x the scale instead of 300 m,
// - the per-frame render lists and the streaming memory are guarded (the scale backs off when they run short),
// - model streaming loads more than one model a frame (Enhancements > World > Model streaming),
// - the `lcsdraw` control command reports all of it (pop-in counters included).
// 100% with "Original" fog and "PS2 pace" streaming is the PS2 game exactly: every hook passes through.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace lcs::drawdist
{
    // Before settings::load (main.cpp, LCS runs only): the LCS rows and the draw distance's longer range.
    void registerSettings();
    // At start-up (applyLcs).
    void install(PS2Runtime &runtime);
    // Game thread, at the frame kick (lcs_world.cpp): the memory governor and the counters.
    void onFrame(uint8_t *rdram);
    // The LOD scale to write this frame (1.0 = the PS2's): the setting, backed off when memory or lists run short.
    float lodScale();
    // The draw distance setting's range on LCS (percent).
    constexpr int kMaxPercent = 1000;
    // [tone] Both cities (dualworld.cpp): while Liberty City is the city behind, its draw distance is at least this
    // (percent; 0: the setting as it is). The setting itself (and vcs.ini) is left unchanged. Any thread.
    void setFloor(int percent);
}
