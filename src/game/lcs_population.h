// [lcs] Liberty City Stories' side of Enhancements > World > Traffic density and Pedestrian density (VCS's:
// game/vcs_world_detail.cpp), and of the "cars in view are kept" guard VCS's draw distance has. The settings are VCS's
// (registered by vcs_world_detail.cpp); this reads their values (and Draw distance's, which lcs_world.cpp applies).
#pragma once

class PS2Runtime;

namespace lcs::population
{
    // At start-up (applyLcs): wraps LCS's car and ped spawners, its random-number generator (for the two spawn-chance
    // draws only), PossiblyRemoveVehicle, RemoveDistantCars (once a frame) and CWorld::Remove (the vanish log).
    void install(PS2Runtime &runtime);
}
