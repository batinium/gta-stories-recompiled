// [lcs] Liberty City Stories' frame rate and game timing: Display > Game timing and Frame rate cap, as VCS's
// (game/vcs_framerate.h, whose settings and state these hooks read).
#pragma once

class PS2Runtime;

namespace lcs::framerate
{
    // At start-up (applyLcs).
    void install(PS2Runtime &runtime);
}
