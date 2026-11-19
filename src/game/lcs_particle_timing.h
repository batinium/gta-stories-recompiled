// [lcs] Liberty City Stories' particles on the time step above 30 fps (as game/vcs_particle_timing.h for VCS).
#pragma once

class PS2Runtime;

namespace lcs::particle_timing
{
    // At start-up (applyLcs): wraps CParticle::Update, AddParticle (both) and AddJetExplosion. Before
    // lcs::effects_timing::install, which chains onto AddParticle.
    void install(PS2Runtime &runtime);

    // "Reduce Post Processing Effects" (enhance.reduce_postfx) on: the particles and effects as before (per frame).
    bool reduced();
}
