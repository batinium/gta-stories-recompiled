// [android] The side panel's native side in the game (docs/ANDROID.md "Side panel"): the Java panel
// (io.github.batinium.vcsrecomp.SidePanel) reads and changes the game's settings through the settings registry
// (settings/settings.h, so vcs.ini stays the one store), asks for a screenshot, the free camera, the perf CSV, the game's
// pause menu, and reads the pad to move its focus. Only in libmain.so (the smoke test has no settings and no panel).
#pragma once

namespace vcs_android
{
    // Main thread, once per host frame inside the runtime's UI hook (main.cpp), after the menus: the panel's queued
    // requests (settings changes, actions) run here, where the settings menu changes them too; the game's state for
    // the controls is published (touch_controls.h noteGameState); a controller's Home / Select + Start hold is seen.
    void panelHostFrame();
}
