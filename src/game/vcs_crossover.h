// [crossover] Liberty City Stories' real vehicles in Vice City Stories (docs/STORIES.md "Cross-game vehicles"): the
// VCS side of game/lcs_crossover.h.
//
// tools/stories_pack.py reads LCS's vehicles ONCE from the player's own LCS disc into VCS's mods folder
// (<data>/mods/stories_lcs_vehicles: PC .dff / .txd per vehicle, vehicles.txt with the class, label, handling, donor and
// paint). Each one is bound ON DEMAND to a NEW VCS model id (281-297: VCS's vehicles end at 280, 281-309 are empty) -
// a copy of its donor's model info (the nearest VCS vehicle of its class) in guest scratch, a texture list slot and a
// handling record of its own (VCS's model info points at its record: no hook needed), its LCS paint - converted by the
// mod loader on the donor's stock model and served for the new id's streaming entries. CText::Get answers its LCS
// name. An id whose vehicles are all gone has its model unloaded; with every id in use, the oldest LCS model's
// vehicles are deleted for the next. Nothing here runs without the pack. VCS only.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class PS2Runtime;
struct R5900Context;

namespace vcs::crossover
{
    constexpr int kIdBase = 100000; // trainer / handover ids: kIdBase + LCS's model id (as lcs::crossover::kIdBase)

    struct Vehicle
    {
        int id = 0;                       // the other game's (LCS's) model id
        std::string cls, label, dff, txd; // car / bike / boat / heli / plane; "Banshee"; paths under mods/
        std::vector<std::pair<std::string, std::string>> handling;
        int donor = 0;                    // the nearest VCS model of its class (0: the class's default)
        std::vector<std::string> colours; // paint pairs "r,g,b,r,g,b"
    };

    void install(PS2Runtime &runtime);
    const std::vector<Vehicle> &vehicles();
    const Vehicle *find(int otherId);
    // Game thread: the VCS model id `otherId` is (now) bound to, 0 with `why`; every id in use: `retry` set while one is
    // freed (with `evict`: the oldest LCS model's vehicles to delete, DELETE_CAR handles)
    int bind(uint8_t *ram, int otherId, std::string &why, std::vector<int32_t> *evict = nullptr, bool *retry = nullptr);
    int boundOther(int vcsModel); // the LCS model a new id holds (0: none)
    bool isNewId(int vcsModel);   // one of the new ids (281-297)
    void onFrame(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime);
}
