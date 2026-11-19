// Save anywhere (fork addition, docs/TRAINER.md "Save anywhere"): a normal memory-card save made where the player
// stands, not only at a safehouse - through the game's own save code, so it is the game's format: Load Game lists and
// loads it, and a real PS2 card takes it.
//
// - "Save game here" opens the game's own Save menu (script command ACTIVATE_SAVE_MENU, 0x0260: what a safehouse's
//   script runs), with its 8-slot list, the slots' names and its overwrite question.
// - "Quick save" writes one fixed slot (kQuickSlot) without the menu: the calls the menu's "save to slot" action
//   FUN_00108128 makes - card scan FUN_00276bf0, the saving flag 0x48A664, FUN_002778a8(card manager, slot), the
//   flag down, the scan again - chained as guest calls inside one scePadRead call (between frames), so the world does
//   not move while it saves (game-internals.md 10b).
// - Both refuse while saving would not be safe (a mission, a script camera / cutscene, a wanted level, in a vehicle,
//   not standing on foot, falling / going through a door, the free camera): see whyNot().
// - Position: a save made here also gets a line in <data>/save_anywhere.txt (slot, the save's play-time stamp, where
//   the player stood). After a load whose play-time stamp (0x488284, which the load sets from the save) matches one,
//   the player is put back there if the game's load put him more than 10 m away. The stamp is the save's own, so
//   a line never applies to another save; a safehouse save in that slot drops the slot's line.
//
// Quick load is not offered: the game loads only from its frontend (the pause menu's Load Game), at world init.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class PS2Runtime;

namespace vcs::saveanywhere
{
    constexpr int kQuickSlot = 8; // 1..8, as the game's menus count

    // Main thread, before runtime.run() (applyVcs): the hooks (scePadRead, the slot save, the load).
    void install(PS2Runtime &runtime);
    // Game thread, at the frame kick, after the trainer's (the trainer starts its scripts there first).
    void onGameFrame(uint8_t *ram);
    // Main thread, each host frame (both games): the save pictures (game/save_shots.h; <data>/save_shots, the launchers' Saves lists)
    void onHostFrame();
    // Any thread: a load was started (the boot's or the menu's) and the game has not yet played its first frames from it
    // (dualworld.cpp: the background Vice City is not ready to take the player until then)
    bool loading();

    // Any thread (the trainer menu, the Android side panel): requests run on the game thread's next frame kick; the
    // outcome is a trainer notice.
    void saveHere();  // the game's Save menu, here
    void quickSave(); // slot kQuickSlot, overwriting it
    std::string whyNot(); // "" when saving is possible now (as of the last frame kick), else the reason
    bool busy();          // a request is being carried out
    bool restoreAfterLoad();
    void setRestoreAfterLoad(bool on);

    struct Slot
    {
        int slot = 0;     // 1..8
        std::string name; // the name the game shows (the last mission passed), "" when empty
    };
    std::vector<Slot> slots(); // the card's slot files (the host folder), every slot 1..8
}
