// [lcs] Free aim with the mouse in Liberty City Stories (docs/CONTROLS.md "Free aim"; VCS's is game/vcs_aim.h).
//
// LCS has the same free aim as VCS - VCS inherited it: R1 held, then L3, is a lock-on to an invisible dummy object
// (the player's +0xCC0, made at his creation) that the left stick moves, with the white circle drawn in the middle of
// the screen and the lock-on camera centred on the dummy (lcs_aim.cpp). The PC way is VCS's: the pad read enters it
// (R1 + L3 in one report), the mouse / right stick turn the dummy around Toni and the camera with it, the world runs
// with the free-aim flag cleared (Toni moves as he does locked on with that gun instead of standing still), the camera
// and the HUD run with it set, and the player's shots test their line from the camera through the circle.
// VCS_FREEAIM=0 installs none of it (the reference), as for VCS.
//
// vcs_input.cpp asks here instead of game/vcs_aim.h when the game is LCS (game::isLcs); the Input / Output are VCS's.
// As VCS's, the camera's blends into / out of free aim are cut to 100 / 200 ms and every gun's aim ends in a blend.
// Not ported (not looked for in LCS): a script's own free aim (VCS's drive-by mode and forced free aim), a gun on a
// vehicle, and the follow camera's first-frame hand-back after an aim (VCS's cameraUpdated).
#pragma once

#include "game/vcs_aim.h"

#include <cstdint>

class PS2Runtime;

namespace lcs::aim
{
    void install(PS2Runtime &runtime); // applyLcs

    vcs::aim::Output update(const vcs::aim::Input &in); // game thread, once per game frame (vcs::aim::update's contract)
    bool gunInHand(); // a weapon the game's free aim takes (a gun: not melee, thrown or a first-person weapon)
    bool aiming();    // the free-aim lock is held, as of the last pad read
}
