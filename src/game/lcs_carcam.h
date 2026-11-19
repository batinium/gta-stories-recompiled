// [lcs] The mouse on Liberty City Stories' camera behind a vehicle: game/vcs_carcam.h's behaviour (the mouse turns it,
// it stays where it was put while the mouse moves, then eases back behind the vehicle after Keyboard > Car camera
// return), on LCS's camera (lcs_carcam.cpp). LCS_CARCAM=0 leaves it out (the mouse moves the right stick there).
// In the Rhino, once the mouse has turned the camera, the turret turns toward where the camera looks (2.5 rad/s at
// most; CAutomobile::TankControl 0x13A018) and the camera stays where it was put (no return) until the right stick
// takes it back - the PC way; LCS_TANK_AIM=0 leaves the turret to the stick, as on the PS2.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace lcs::carcam
{
    // applyLcs: hooks LCS's follow-vehicle camera (0x283EB8, CCam::Process's modes 18 / 22).
    void install(PS2Runtime &runtime);

    // vcs::carcam's contract (game/vcs_carcam.h): does the mouse turn this camera mode itself; the mouse's turn since
    // the last pad read (radians, left +, up +); the right stick moved (the game has the camera at once).
    bool turns(uint32_t mode);
    void mouse(float turn, float tilt);
    void stickMoved();
}
