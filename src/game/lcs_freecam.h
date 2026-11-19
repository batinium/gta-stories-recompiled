// [lcs] The trainer's free camera (docs/TRAINER.md "Camera") for Liberty City Stories: VCS's (game/vcs_freecam.h) with
// LCS's camera layout. CCamera::Process 0x28FAC8 runs the active CCam's update, CCam::Process 0x27A380 (0x28FCBC), then
// its height routines 0x287020 (0x28FCD0), then builds the frame's camera from that CCam's Source / Front / Up / FOV.
// While flying the game's update still runs and the height routines are replaced for the active CCam: the free camera
// writes those fields there and returns. LCS_FREECAM=0 leaves it unhooked (the reference).
//
// vcs::freecam::flying() / pose() answer from here when the game is LCS ([lcs] seam in vcs_freecam.cpp), so the input
// code (a pad left alone, the mouse captured: vcs_input.cpp) and the menu work unchanged.
#pragma once

#include "game/vcs_freecam.h"

class PS2Runtime;

namespace lcs::freecam
{
    // From applyLcs (lcs_overrides.cpp).
    void install(PS2Runtime &runtime);

    // Any thread: the trainer's Free camera is on and the game is in play.
    bool flying();
    bool installed();

    vcs::freecam::Pose pose();

    // Any thread (tests: `trainer freecam x y z yaw pitch fov`): the flying camera jumps there at its next update.
    void setPose(float x, float y, float z, float yaw, float pitch, float fov);
}
