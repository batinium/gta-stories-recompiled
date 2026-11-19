// [lcs] Liberty City Stories' world "looks" that move a fixed amount every frame (scrolling LED signs, the water's
// texture drift), at the PS2's rate above 30 fps. See lcs_world_timing.cpp.
#pragma once

class PS2Runtime;

namespace lcs::world_timing
{
    // At start-up (applyLcs).
    void install(PS2Runtime &runtime);
}
