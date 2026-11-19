// [lcs] Liberty City Stories' side of the player's controls (game/vcs_input.h): what the pad reads need to know about
// the game. The bindings, gamepad map, stick options and vibration are vcs_input.cpp's and work for any game; it asks
// here instead of reading VCS's memory when the game is LCS (game::isLcs).
#pragma once

#include <cstdint>

class PS2Runtime;

namespace lcs::input
{
    // At start-up (applyLcs): the shake hooks (driving vibration).
    void install(PS2Runtime &runtime);
    // The Vibration setting "Driving vibration": the engine / road rumble's strength, 0..2 (1: as the game sets it).
    void setDrivingShake(float scale);

    struct State
    {
        bool driving = false;    // the player drives (the Driving keys apply)
        bool footCamera = false; // always false: LCS's cameras the mouse turns are game/lcs_camera.h's (vcs_input.cpp
                                 // asks there before its VCS paths)
        bool paused = false;     // the pause menu or the game's own pause
    };
    State state(const uint8_t *rdram);

    // The active camera's mode for vcs_input's mouse wheel: 4 for the on-foot follow camera, otherwise a value none of
    // VCS's scope / zoom / car-camera paths take (they write VCS's camera layout).
    uint32_t cameraMode(const uint8_t *rdram);

    // The radio of the vehicle the player is in, for vcs_input's mouse wheel (L1 steps through the stations; the
    // game adds the presses to the station a moment after the last one, as VCS).
    struct Radio
    {
        uint32_t vehicle = 0;   // the vehicle (0: on foot)
        uint32_t offset = 0;    // its station byte's offset
        uint32_t positions = 0; // the stations and off that L1 steps through
    };
    Radio radio(const uint8_t *rdram);
}
