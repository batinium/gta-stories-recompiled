// [android] Which physical controller the game reads (docs/ANDROID.md "Controllers"). Upstream's platform layer opens
// the first SDL gamepad and ignores the rest; on a phone the first one can be something that is no controller at all
// (a key device with D-pad keys, a pad's extra keyboard node), and one pad can show as more than one device. Here every
// gamepad SDL reports is looked at: the ones that are no controller are ignored, the rest opened, grouped by
// VID / PID + name, and one is used: Auto (the one last pressed), a chosen pad (Gamepads.java, by VID / PID and name),
// or none (touch only). The platform layer asks activeGamepad() each frame instead of keeping its own.
#pragma once

#include <cstdint>

union SDL_Event;
struct SDL_Gamepad;

namespace vcs_android
{
    // Before SDL_Init: SDL's hints for the Android joystick backend (key-only devices are not joysticks).
    void gamepadsInit();
    // Every SDL event, main thread: gamepads added / removed (true: handled, upstream's cases skip it) and their input
    // (the most recent pad for Auto; false: the event goes on).
    bool gamepadEvent(const SDL_Event &e);
    // The pad the game reads now, or null (none connected, or touch only). Main thread, after the events.
    SDL_Gamepad *activeGamepad();
    void gamepadsShutdown();

    // The choice (Gamepads.java, SharedPreferences "touch_controls": pad_choice, pad_vid, pad_pid, pad_name)
    enum PadChoice : int
    {
        PAD_AUTO = 0,
        PAD_CHOSEN = 1,
        PAD_TOUCH_ONLY = 2,
    };
}
