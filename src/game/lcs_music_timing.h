// [lcs] Liberty City Stories' radio off the air on the time step above 30 fps (as game/vcs_music_timing.h for VCS).
#pragma once

class PS2Runtime;

namespace lcs::music_timing
{
    // At start-up (applyLcs).
    void install(PS2Runtime &runtime);
}
