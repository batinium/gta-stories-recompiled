// [lcs] Liberty City Stories' side of two settings whose VCS work happens at VCS's frame kick (game/vcs_world_detail.h,
// game/vcs_input.h): Enhancements > World > Draw distance (the LOD distance scale) and Game > In the background: Pause.
// The settings are VCS's (registered by vcs_world_detail.cpp / main.cpp); this reads their values.
#pragma once

class PS2Runtime;

namespace lcs::world
{
    // At start-up (applyLcs), after lcs::display (its frame-kick hook chains onto lcs_hud's).
    void install(PS2Runtime &runtime);
}
