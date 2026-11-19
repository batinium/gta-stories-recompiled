// [lcs] Liberty City Stories' cameras turned by the mouse: the on-foot follow camera and the scopes / first-person
// weapon views turn by the mouse itself (as vcs_input.cpp does for VCS), the wheel zooms the scopes that zoom. The
// mouse's maths are vcs_input.cpp's; the camera's layout here is LCS's (TheCamera, CCam: lcs_camera.cpp).
// vcs_input.cpp asks here when the game is LCS (game::isLcs); what this does not take turns the right stick, as before.
#pragma once

#include <cstdint>

namespace lcs::camera
{
    // The active CCam (TheCamera.Cams[ActiveCam]), 0 before the camera exists.
    uint32_t activeCam(const uint8_t *rdram);
    // Its mode (CCam::Mode, u16): 4 the follow camera on foot, 7 / 8 / 34 / 45 / 46 the scopes, 18 behind a vehicle;
    // 0xFFFF without a camera.
    uint32_t mode(const uint8_t *rdram);

    // The on-foot follow camera the player turns (no script camera, controls on, the stick not locked out).
    bool footCamera(const uint8_t *rdram, bool driving, bool paused);

    // Game thread, once per game frame with the mouse's movement since the last one, in radians (`turn`: left +,
    // `tilt`: up +) at the on-foot sensitivity and at the car camera's. True when a camera took it (it is not the right
    // stick then): on foot, a scope, the camera behind a vehicle (game/lcs_carcam.h).
    bool mouse(uint8_t *rdram, float turn, float tilt, float carTurn, float carTilt, bool driving, bool paused);

    // Game thread: a wheel notch in a scope that zooms (in: closer). True when it was one.
    bool wheelZoom(uint8_t *rdram, bool in, bool out);
}
