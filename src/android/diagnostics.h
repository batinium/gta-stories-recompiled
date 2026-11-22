// [android] The game's heartbeat and stall watchdog (docs/ANDROID.md "Diagnostics"): lines in game.log that a crash
// report carries, so a freeze or a device loss can be read without the player doing anything technical.
//   [hb] t=<s> frames=<n> fps=<x> frame_ms_max=<x> thermal=<s> battery=<pct>/<C> mem_free=<MB> game=<g> driver=<d>
//   [watchdog] no frame for <n> s (last hb ...) + one line per native thread (name, state, wchan)
// The phone's side (thermal status, battery, free memory, in the foreground or not) comes from Java (Diag.java) as a few
// numbers every few seconds. VCS_WATCHDOG=0 (files/env.txt) turns the watchdog off; the heartbeat stays.
#pragma once

namespace vcs_android
{
    // Once, from the game's main after env.txt is read. A detached thread that wakes every second; never touches the game.
    void startDiagnostics();
}
