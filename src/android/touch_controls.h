// [android] The on-screen controls' native side (docs/ANDROID.md "Touch controls"): the Java view
// (io.github.batinium.vcsrecomp.TouchControls) decides what each touch holds and sends the result here through JNI as a
// virtual PS2 pad and camera drags. The platform layer (platform_sdl3.cpp) merges them into its input snapshot: the pad
// with a real gamepad's state (both work at once), the drags with the mouse's motion (takeMouseMotion: the mouse camera).
#pragma once

#include "platform/platform.h"

#include <cstdint>

namespace vcs_android
{
    struct TouchPad
    {
        bool on = false;                       // the overlay is shown and takes touches
        uint32_t buttons = 0;                  // 1 << platform::GamepadButton
        float axes[platform::AXIS_COUNT] = {}; // as platform::gamepadAxis: sticks -1..1 (down / right +), triggers 0..1
    };

    // Any thread: the overlay's state as last sent from Java.
    TouchPad touchPad();
    // The overlay's pad as the game's bindings see it (platform_sdl3.cpp merges this one): the overlay sends each PS2
    // button as its default gamepad source (Cross as South...), so once the controller is remapped (Gamepad page,
    // pad.<id>; the panel's Remap screen) each is turned into the source now bound to that PS2 button, and on-screen
    // Cross stays Cross. Index and mask bits: GamepadButton 0..13, 14 the left trigger (L2), 15 the right (R2); a mask of
    // 0 is unbound. Identity until set (side_panel.cpp, each host frame; identity while the menus that read the raw pad
    // are open).
    void setTouchBindings(const uint32_t map[16]);
    TouchPad touchPadBound();
    // Camera drags since the last call, in mouse counts (takeMouseMotion adds them; one consumer).
    void takeTouchLook(float &dx, float &dy);

    // From the platform layer's pumpEvents, for Java to read: whether the game wants the mouse captured now (playing,
    // no menu open: setMouseCaptured), when a drag on the screen is the camera; whether SDL has a real gamepad open
    // (Auto mode hides the overlay then).
    void noteLookWanted(bool wanted);
    void noteRealGamepad(bool connected);

    // The overlay's settings button (no F2 key on a phone): true once per tap, read by the settings menu to open or
    // close itself (ui/settings_menu.cpp).
    bool takeMenuToggle();
    // The overlay's trainer button (no F5 key on a phone): true once per tap, read by the trainer to open or close
    // itself (ui/trainer_menu.cpp).
    bool takeTrainerToggle();

    // ---- the side panel (SidePanel.java, src/android/side_panel.cpp; docs/ANDROID.md "Side panel") ----
    // While it is open the game reads no pad (the overlay's nor a real one's): the panel has them. After it closes the
    // pad stays blocked until every button is let go, so the press that closed it is not the game's.
    void setPanelOpen(bool open);
    bool panelOpen();
    // The panel's "Pause the game": the game thread holds (game/vcs_input.cpp, as Game > In the background's pause) and
    // the sound is muted.
    void setHostPaused(bool paused);
    bool hostPaused();
    // The real gamepad as SDL reads it (snapshotGamepad, before the merge and the panel's block): 1 << GamepadButton,
    // plus kGuideBit for its Home / Guide button; for the panel's focus and its open gestures.
    constexpr uint32_t kGuideBit = 1u << 16;
    void noteRealPad(uint32_t buttons, const float axes[platform::AXIS_COUNT]);
    uint32_t realPad(float axes[platform::AXIS_COUNT]);
    // A key held for `frames` host frames as if pressed on a keyboard (no F-keys on a phone): Escape for the game's
    // pause menu, F4 for the perf CSV. Any thread; pumpEvents applies them (applyInjectedKeys) to its snapshot.
    void injectKey(int key, int frames);
    void applyInjectedKeys(bool *keys, int count, bool frameStart);
    // The game's state the Java side lays the controls out by (main thread, each host frame: side_panel.cpp).
    enum GameStateBits : int
    {
        STATE_IN_VEHICLE = 1,
        STATE_AIMING = 2,      // free aim, or R1 (the lock-on) held
        STATE_SETTINGS_MENU = 4,
        STATE_TRAINER = 8,
        STATE_FREE_CAMERA = 16,
        STATE_GAME = 32,       // the game runs (not the smoke test)
        STATE_PAUSE_MENU = 64, // the game's own pause (or save) menu is up
        STATE_NEAR_VEHICLE = 128, // on foot, an enterable vehicle in reach: Triangle shows a door
    };
    void noteGameState(int bits);
    // The game's vibration as last asked for (platform::gamepadRumble): low << 16 | high, 0..65535 each, for the phone's
    // vibrator (Rumble.java).
    void noteRumble(uint32_t packed);
}
