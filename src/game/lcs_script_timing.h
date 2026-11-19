// [lcs] Liberty City Stories' mission scripts that add a fixed amount to a meter on every pass of a `WAIT 0` loop -
// once a frame - put on the time step above 30 fps (as game/vcs_script_timing.h for VCS). See lcs_script_timing.cpp.
#pragma once

class PS2Runtime;

namespace lcs::script_timing
{
    // At start-up (applyLcs).
    void install(PS2Runtime &runtime);
}
