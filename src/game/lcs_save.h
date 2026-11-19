#pragma once
// [lcs] Saving anywhere, quick save and quick load for Liberty City Stories (SLUS-21423), the counterpart of VCS's
// game/vcs_save_anywhere.h. Everything goes through the game's own memory card code, so a save made here is the
// game's own slot file: the pause menu's Load Game lists it and the boot loads it.
//
// - Save game here: what a safehouse's ACTIVATE_SAVE_MENU does (script handler at 0x22A820): the frontend's
//   "save menu" flag FrontEndMenuManager+0x47B goes up, the player's move / turn speed to 0; the game's own Save
//   screen with its 8 slots and questions opens.
// - Quick save (F9): the frontend's own "save to slot" calls (0x3396E8..0x339714: card scan, saving flag, the slot
//   save, the flag down), chained as guest calls inside one scePadRead call (between frames), into slot kQuickSlot.
// - Quick load (F10): the frontend's Load Game action (0x33978C..0x3397D4: the slot's check, the load request the
//   main loop acts on at the start of the next frame), from slot kQuickSlot.
// - Saving is refused while it would not be safe: see whyNot().
// - VCS_LOAD_SLOT=<1..8> (as VCS's): the boot's load takes that slot instead of the newest.
//
// Control channel: `lcssave here | quick [slot [force]] | load [slot] | state | slots` ("force": tests save without
// the safety checks).

#include <cstdint>
#include <string>
#include <vector>

class PS2Runtime;

namespace lcs::save
{
    constexpr int kQuickSlot = 8; // 1..8, as the game's menus count

    // Main thread, before runtime.run() (applyLcs): the hooks.
    void install(PS2Runtime &runtime);

    // Any thread (a menu, the control channel): carried out on the game thread at the next pad read; the outcome is
    // a notice.
    void saveHere();
    void quickSave(int slot = kQuickSlot);
    void quickLoad(int slot = kQuickSlot);
    std::string whyNot(); // "" when saving is possible now (as of the last frame), else the reason
    bool busy();

    struct Slot
    {
        int slot = 0;     // 1..8
        std::string name; // the name the game shows (the last mission passed), "" when empty
    };
    std::vector<Slot> slots(); // the card's slot files (the host folder), every slot 1..8
}
