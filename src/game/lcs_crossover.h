// [crossover] Vice City Stories' real vehicles in Liberty City Stories (docs/STORIES.md "Cross-game vehicles").
//
// tools/stories_pack.py reads VCS's vehicles ONCE from the player's own VCS disc into LCS's mods folder
// (<data>/mods/stories_vcs_vehicles: PC .dff / .txd per vehicle, vehicles.txt with the class, label and handling).
// Here every one of them can be spawned (the trainer's vehicle list, `trainer spawn 1000xx`) and kept at the border:
// a VCS vehicle is bound ON DEMAND to a NEW LCS model id (217-236: VC's last vehicle ids, empty in LCS) - a copy of its
// donor's model info (the nearest LCS vehicle of its class) in guest scratch, a texture list slot and a handling record
// of its own - converted by the mod loader on the donor's stock model (vcs_mods.cpp bindVehicle: converted once,
// cached) and served for the new id's streaming entries. Hooks give its vehicles their own handling record, the donor's
// engine sounds and its VCS name on the HUD. 20 VCS vehicles exist at once (cars, bikes, boats, helicopters, planes
// alike); a new id whose model is unloaded again takes the next one. When all are in use (or the hooks are off:
// STORIES_NEWIDS=0), the old pool: a pool slot of its class (a model whose handling record is its own, not used by
// traffic: 6 cars, 2 bikes, 2 boats) that gets its stock values back once unloaded.
//
// Nothing here runs without the pack. LCS only.
#pragma once

#include <cstdint>

class PS2Runtime;
struct R5900Context;
#include <string>
#include <utility>
#include <vector>

namespace lcs::crossover
{
    // Trainer / handover ids of the pack's vehicles: kIdBase + VCS's model id (VCS's Deluxo 187 -> 100187)
    constexpr int kIdBase = 100000;

    struct Vehicle
    {
        int id = 0;                // the other game's model id
        std::string cls, label;    // car / bike / boat / heli / plane; "Deluxo"
        std::string dff, txd;      // paths under mods/
        std::vector<std::pair<std::string, std::string>> handling;
        int donor = 0;             // the nearest LCS model of its class (the pack's; 0: the class's default)
        std::vector<std::string> colours; // its paint colour pairs "r,g,b,r,g,b" (VCS's palette, the pack's)
    };

    // applyLcs: the hooks for new model ids (handling record, sounds, HUD name)
    void install(PS2Runtime &runtime);

    // The pack's vehicles (read once; empty without the pack or with VCS_MODS=0). Any thread.
    const std::vector<Vehicle> &vehicles();
    const Vehicle *find(int otherId);

    // Game thread: the slot `otherId` is (now) bound into, 0 with `why` when none is free or it can't be converted.
    // Every id in use: `retry` set when one is being freed - a model with no vehicle left (unloaded by onFrame) or,
    // with `evict`, the oldest VCS model's vehicles (none the player's) to delete (pool handles: DELETE_CAR) - try
    // again a frame later.
    int bind(uint8_t *ram, int otherId, std::string &why, std::vector<int32_t> *evict = nullptr, bool *retry = nullptr);

    // The other game's model id an LCS slot holds now (0: none, its own model). Any thread.
    int boundOther(int lcsModel);

    // Game thread, once a frame: slots whose model is unloaded again get their stock values back.
    // A new id with no vehicle of it left for 3 s (or evicted by bind) has its model unloaded: the id is free.
    void onFrame(uint8_t *ram, R5900Context *ctx, PS2Runtime *runtime);
}
