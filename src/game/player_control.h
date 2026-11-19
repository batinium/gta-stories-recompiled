// [lcs] Whether the player has control right now, in either game (game/game_addrs.h for the shared globals): a game
// runs (script space set), the player ped exists, CPad 0's DisablePlayerControls is 0 (scripted scenes and cutscenes
// set it), and the active camera is the player's - following the ped on foot (mode 4 in both games), or behind the
// vehicle the player is in (any mode but the fixed / fly-by / cutscene ones, 14-17). Reads guest memory from any thread
// (a few aligned loads; a torn read only delays the answer by a frame).
#pragma once

#include <cstdint>

// (namespace player, not game: src/main.cpp has a function game())
namespace player
{
    bool hasControl(const uint8_t *rdram);
}
