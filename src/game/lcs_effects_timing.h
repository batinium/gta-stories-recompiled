// [lcs] Liberty City Stories' visual effects that count frames, put on the PS2's 30 frames a second above 30 fps (as
// game/vcs_effects_timing.h for VCS).
#pragma once

class PS2Runtime;

namespace lcs::effects_timing
{
    // At start-up (applyLcs), after lcs::population::install and lcs::particle_timing::install (it chains onto their
    // rand and AddParticle hooks).
    void install(PS2Runtime &runtime);
}
