// [lcs] Drive-by aiming with the mouse in Liberty City Stories (the PC way of VCS's free aim, game/vcs_aim.h, on LCS's
// own drive-by).
//
// LCS's player drive-by (lcs_driveby.cpp): with a drive-by weapon in hand (an SMG: weapon info +0x68 == 5) the player
// looks left or right in a car or on a bike (L2 / R2: CCam::Process 0x27A380 calls LookLeft 0x278F90 / LookRight
// 0x279778, which put the camera on the far side of the vehicle looking across it and set CCam +0x30 / +0x34) and
// fires (Circle) out of that side: the vehicle's drive-by (cars 0x2AF200, 0x2AF628; bikes 0x2AF938, which also fire
// straight ahead when not looking) calls CWeapon::FireFromCar 0x2A30F0 -> FireInstantHitFromCar 0x2AC710: a shot
// from the hand along the vehicle's side (or forward), a random spread, then DoDriveByAutoAiming 0x2AE1C8 bends it
// toward a ped near the line. Its direction is fixed to the vehicle: the stick does not aim it.
//
// Here the mouse (and the right stick) aims it: while looking out of a side with a drive-by weapon they turn that look
// camera about the point it looks at (95 deg either way of the side, 15 deg down to 25 deg up) before the game keeps it
// out of walls (CCamera::AvoidTheGeometry), the view follows the aim one to one, a little over the vehicle (so the
// middle of the screen is clear of its roof), the game's own aiming circle (its free aim's) is drawn in the middle,
// and the shot goes down that line through the middle of the screen (from beside the gun, as free aim's shots:
// lcs_aim.cpp), instead of along the side plus the spread and the auto-aim. On a bike's straight-ahead drive-by the
// shot takes the heading of the camera behind it (which the mouse turns: lcs_carcam.cpp), within 60 deg of the bike's.
// Toni's gun arm follows the aim: the drive-by poses are fixed clips per side (no aim angle), so each frame the
// vehicle's drive-by update first runs the game's own arm IK (CPedIK::PointGunInDirectionUsingArm, what on-foot aiming
// uses) toward the circle, its limits widened for the call, the forearm and hand then straightened along the arm (the
// clip's elbow is bent), and what it is asked is corrected by how far the arm drawn last frame missed (the drive-by
// clips tilt the clavicle the IK measures from): within 2 deg of the aim, up to 140 deg either way of ahead.
// LCS_DRIVEBY_ARM=0 leaves the clip's arm.
// LCS_DRIVEBY_AIM=0 installs none of it (the game's own drive-by); LCS_DRIVEBY_CROSSHAIR=0 no aiming circle;
// LCS_DRIVEBY_LIFT=<m> how far over the game's look point the view looks (0.7; 0 = through it, as the game);
// LCS_DRIVEBY_BIKE_FORWARD=0 leaves the bike's straight-ahead shots alone.
#pragma once

#include <cstdint>

class PS2Runtime;

namespace lcs::driveby
{
    void install(PS2Runtime &runtime); // applyLcs

    // Game thread, from lcs::camera::mouse once per game frame (radians, left +, up +): true when the drive-by look
    // camera took the mouse (it is not the camera behind the vehicle then).
    bool mouse(uint8_t *rdram, float turn, float tilt, bool paused);

    // Any thread, at each pad read (vcs_input.cpp): the right stick as the game gets it (dead zone, invert and camera
    // speed applied; -1..1, x right +, y down +). It aims the drive-by while the look camera is up.
    void stick(float x, float y);

    // [android] Any thread: the player looks out of a side of his vehicle with a drive-by weapon (the look camera the
    // mouse / stick aim), as of the last few game frames. The side panel's "aiming" state (the gyro's While aiming).
    bool aiming();
}
