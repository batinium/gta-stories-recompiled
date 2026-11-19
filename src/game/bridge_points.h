// [stories] Player position for choosing where the both-cities bridge lands (docs/STORIES.md "Choosing where the bridge
// lands"): the F3 panel's `POS x y z` line, and the trainer's "Note position for the bridge" (appends a line to
// <data folder>/stories/bridge_points.txt, the folder of the game's memory card and ELF cache).
#pragma once

#include <cstdint>
#include <string>

namespace game::bridgepoints
{
    // The player's (or his vehicle's) world position from the game's RAM; false before there is a player.
    bool playerPosition(const uint8_t *ram, float out[3]);
    // `POS -1833.0 -656.0 14.0` and, in Vice City with both cities on (dualworld), `  (LCS 1967.0 -656.0)`.
    std::string positionLine(const float p[3]);
    // Appends `<timestamp> <LCS|VCS> x y z (LCS x y z)` to stories/bridge_points.txt; false when it cannot be written.
    bool note(const float p[3]);
}
