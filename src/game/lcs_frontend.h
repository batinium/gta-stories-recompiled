// [lcs] Liberty City Stories' pause menu with the keyboard and the mouse, the PC way (as game/vcs_frontend.h for VCS):
// Escape opens it (Start's key), then Enter or the left button selects (Cross), Escape or the right button goes back
// (Triangle; from the top page that resumes the game), a wheel notch steps up / down (the D-pad); the arrow keys are
// the D-pad already. The pointer picks what is under it: a tab of the tab bar (the D-pad steps there), an entry of the
// page that has the focus (selected); a click activates it. On the map page the map's cursor follows the pointer, the
// left button drags the map, a click is Cross and the wheel zooms (about the pointer); on Stats and Brief the wheel
// scrolls the text. LCS's menu is the GTA III-style CMenuManager (lcs_frontend.cpp: where its tabs, entries, the map
// and the scrolling pages are). LCS_MENU_MOUSE=0: no pointer (buttons and wheel only, the wheel the D-pad).
// vcs_input.cpp asks here instead of game/vcs_frontend.h when the game is LCS (game::isLcs).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace lcs::frontend
{
    void install(PS2Runtime &runtime); // applyLcs

    bool open(); // the pause menu is up; any thread

    // Main thread, once a host frame (vcs::frontend::onHostFrame's contract): the cursor over the menu.
    void onHostFrame();

    // Game thread, at each pad read (vcs::frontend::padButtons' contract): the buttons the menu adds (Cross for
    // Enter / the left button, Triangle for Escape / the right button, Up / Down for the wheel), worked out once per
    // game frame. `user`: the player's own buttons.
    uint16_t padButtons(uint16_t user, bool blocked);

    // Game thread: `key` is the menu's (Escape, Enter, the mouse buttons and wheel while it is up, or still held from
    // then): no binding then.
    bool ownsKey(int key);
}
