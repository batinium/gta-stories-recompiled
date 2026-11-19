// [lcs] Liberty City Stories' vehicle physics that acts once a frame, on the time step above 30 fps (as
// game/vcs_physics_timing.h for VCS).
#pragma once

class PS2Runtime;

namespace lcs::physics_timing
{
    // At start-up (applyLcs).
    void install(PS2Runtime &runtime);
}
