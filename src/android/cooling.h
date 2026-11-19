// [android] The game's side of power and heat (docs/ANDROID.md "Power and heat"): two settings (Display > Phone heat)
// and the automatic step-down - the frame rate capped to 30 while the phone is hot (thermal status moderate or worse,
// or a headroom forecast near throttling), back to the player's cap once it has been cool for a minute. Only in
// libmain.so (the smoke test has no settings); the readings come from android/power.h.
#pragma once

namespace vcs_android
{
    // registerSettings (main.cpp), with the other settings: android.hot, android.adpf
    void registerPowerSettings();
    // Main thread, once per host frame inside the runtime's UI hook (main.cpp)
    void coolingHostFrame();
}
