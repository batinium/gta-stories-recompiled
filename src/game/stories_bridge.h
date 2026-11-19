// Stories: the bridge (both cities, experimental). A drivable deck from Portland's east docks (LCS) out over the sea
// to Vice City's west shore at Escobar International (VCS = LCS + the dual-world offset), the same deck in both games
// at matching places, so the border handover (dualworld.cpp) happens on it.
//
// How: the games' world collision is the col models of the building entities in the 40 m (LCS) / 80 m (VCS) sector
// lists - cars (CAutomobile::ProcessEntityCollision -> CCollision::ProcessColModels, the wheel lines included), peds
// and line tests all go through them. The bridge is a few more of those entities, made in guest memory: each one a
// copy of a real CBuilding, its own box-only col model (deck + two kerbs), its model index a free slot of the model
// info table pointing at a copy of a real building's model info with that col model. Linked into the sector lists it
// covers (list 0, buildings), so the engine's own code collides with it; no collision query is hooked.
// The deck is drawn by our renderer (runtime/gs/gs_bridge.h): a textured strip with kerbs, in each game's own frame.
//
// Switches (environment): STORIES_BRIDGE=0 none of it (default on under STORIES_DUALWORLD; nothing without it).
// Control command `bridge`: state (segments, links, relinks, the player's place on the deck).
#pragma once

#include <cstdint>

namespace bridge
{
    // From dualworld::install (both games, either role): reads the switch, registers the command and the draw source.
    void install(float offX, float offY, float offZ);
    // Game thread, each game frame (dualworld::onGameFrame): builds the deck once the world is up, keeps it linked.
    // frameTag: this game frame's tag (gsdw::setFrameTag, set just before): its projection is kept under it
    void onGameFrame(uint8_t *rdram, uint64_t frameTag);
    bool active();
    // [bridge4] the road centre line y at an LCS x (the route bends: Vice City end lies STORIES_BRIDGE_VCS_DY south)
    float centerY(float lcsX);
}
