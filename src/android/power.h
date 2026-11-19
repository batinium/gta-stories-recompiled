// [android] Power and heat (docs/ANDROID.md "Power and heat"): an ADPF performance hint session for the app's busy
// threads, and the phone's thermal status / headroom for the game's step-down (src/android/cooling.cpp) and the log.
//
// ADPF (android/performance_hint.h, Android 13+): one session over the main thread (window, presenting), the game
// thread (the EE), the render worker and the GS stage, found by name in /proc/self/task once they exist. Its target is
// the host frame period; each host frame reports the busiest of those threads' CPU time since the last report, so the
// system can pick the lowest clocks that still keep every thread inside a frame (instead of the governor's guess from
// load). Android 15+: the session also prefers power efficiency while the game is cooling down.
//
// Thermal (android/thermal.h): the status (none ... shutdown) once a second, the headroom (10 s forecast, 1.0 = where
// the system starts throttling) every 5 s - it is NaN when asked more often.
//
// Everything is looked up with dlsym (libandroid.so), so a phone without a piece just goes without it. In vcs_platform:
// the smoke test has it too. Main thread unless said otherwise.
#pragma once

#include <cstdint>

namespace vcs_android
{
    // Once per host frame (platform::waitNextFrame, before its sleep), with the host frame period.
    void powerHostFrame(uint64_t periodNs);
    // The ADPF session on / off (the "Performance hints" setting; VCS_ADPF=0 turns it off for a run). On by default.
    void setPerformanceHints(bool on);
    // Ask the session to prefer power efficiency (Android 15+; a no-op before): the game is cooling down.
    void setPreferPowerEfficiency(bool on);

    // Any thread: the last thermal readings. Status: AThermalStatus (-1 unknown, 0 none, 1 light, 2 moderate, 3 severe,
    // 4 critical, 5 emergency, 6 shutdown); headroom NaN when unknown.
    int thermalStatus();
    float thermalHeadroom();

    // Any thread: the game is cooling down (cooling.cpp: the frame rate capped to 30 while the phone is hot), for the
    // stats overlay.
    void setCooling(bool on);
    bool cooling();

    // Any thread: the panel's refresh rate as the activity last saw it (Display.getRefreshRate, sent at the start and on
    // every display change: SDL reads it only when the surface changes, so a mode switch went unseen); 0 = not known.
    float displayRefreshHz();
}
