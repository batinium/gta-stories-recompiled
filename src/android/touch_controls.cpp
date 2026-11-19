// [android] The JNI bridge of the on-screen controls (touch_controls.h): the Java view's virtual pad and camera drags in,
// the game's state the view needs (camera drag wanted, a real gamepad) out. Atomics and one small lock: the view calls
// from Android's UI thread, the platform layer reads on the main thread, the game thread reads the pad.
#include "android/touch_controls.h"

#include <jni.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace vcs_android
{
    namespace
    {
        std::atomic<bool> g_on{false};
        std::atomic<uint32_t> g_buttons{0};
        std::atomic<float> g_axes[platform::AXIS_COUNT]{};
        std::mutex g_lookMutex;
        float g_lookX = 0.0f, g_lookY = 0.0f;
        std::atomic<bool> g_lookWanted{false};
        std::atomic<bool> g_realGamepad{false};
        std::atomic<bool> g_menuToggle{false};
        std::atomic<bool> g_trainerToggle{false};
        std::atomic<bool> g_panelOpen{false};
        std::atomic<bool> g_hostPaused{false};
        std::atomic<uint32_t> g_realButtons{0};
        std::atomic<float> g_realAxes[platform::AXIS_COUNT]{};
        std::mutex g_keyMutex;
        struct HeldKey
        {
            int key, frames;
        };
        HeldKey g_heldKeys[8]{};
        int g_heldCount = 0;
        std::atomic<int> g_gameState{0};
        std::atomic<uint32_t> g_rumble{0};
    }

    TouchPad touchPad()
    {
        TouchPad p;
        p.on = g_on.load(std::memory_order_relaxed);
        if (!p.on)
            return p;
        p.buttons = g_buttons.load(std::memory_order_relaxed);
        for (int a = 0; a < platform::AXIS_COUNT; ++a)
            p.axes[a] = g_axes[a].load(std::memory_order_relaxed);
        return p;
    }

    namespace
    {
        std::atomic<bool> g_bindSet{false};
        std::atomic<uint32_t> g_bind[16]{};
    }

    void setTouchBindings(const uint32_t map[16])
    {
        for (int i = 0; i < 16; ++i)
            g_bind[i].store(map[i], std::memory_order_relaxed);
        g_bindSet.store(true, std::memory_order_release);
    }

    TouchPad touchPadBound()
    {
        TouchPad p = touchPad();
        if (!p.on || !g_bindSet.load(std::memory_order_acquire))
            return p;
        uint32_t in = p.buttons & ((1u << platform::GAMEPAD_BUTTON_COUNT) - 1u);
        if (p.axes[platform::AXIS_LEFT_TRIGGER] > 0.5f)
            in |= 1u << 14;
        if (p.axes[platform::AXIS_RIGHT_TRIGGER] > 0.5f)
            in |= 1u << 15;
        uint32_t out = 0;
        for (int i = 0; i < 16; ++i)
            if (in >> i & 1u)
                out |= g_bind[i].load(std::memory_order_relaxed);
        p.buttons = out & ((1u << platform::GAMEPAD_BUTTON_COUNT) - 1u);
        p.axes[platform::AXIS_LEFT_TRIGGER] = out >> 14 & 1u ? 1.0f : 0.0f;
        p.axes[platform::AXIS_RIGHT_TRIGGER] = out >> 15 & 1u ? 1.0f : 0.0f;
        return p;
    }

    void takeTouchLook(float &dx, float &dy)
    {
        std::lock_guard<std::mutex> lock(g_lookMutex);
        dx = g_lookX;
        dy = g_lookY;
        g_lookX = g_lookY = 0.0f;
    }

    void noteLookWanted(bool wanted) { g_lookWanted.store(wanted, std::memory_order_relaxed); }
    void noteRealGamepad(bool connected) { g_realGamepad.store(connected, std::memory_order_relaxed); }
    bool takeMenuToggle() { return g_menuToggle.exchange(false, std::memory_order_relaxed); }
    bool takeTrainerToggle() { return g_trainerToggle.exchange(false, std::memory_order_relaxed); }

    void setPanelOpen(bool open)
    {
        if (g_panelOpen.exchange(open, std::memory_order_relaxed) != open)
            std::fprintf(stderr, "[panel] %s\n", open ? "open" : "closed");
    }
    bool panelOpen() { return g_panelOpen.load(std::memory_order_relaxed); }

    void setHostPaused(bool paused)
    {
        if (g_hostPaused.exchange(paused, std::memory_order_relaxed) != paused)
            std::fprintf(stderr, "[panel] game %s\n", paused ? "paused" : "resumed");
    }
    bool hostPaused() { return g_hostPaused.load(std::memory_order_relaxed); }

    void noteRealPad(uint32_t buttons, const float axes[platform::AXIS_COUNT])
    {
        for (int a = 0; a < platform::AXIS_COUNT; ++a)
            g_realAxes[a].store(axes[a], std::memory_order_relaxed);
        g_realButtons.store(buttons, std::memory_order_relaxed);
    }

    uint32_t realPad(float axes[platform::AXIS_COUNT])
    {
        for (int a = 0; a < platform::AXIS_COUNT; ++a)
            axes[a] = g_realAxes[a].load(std::memory_order_relaxed);
        return g_realButtons.load(std::memory_order_relaxed);
    }

    void injectKey(int key, int frames)
    {
        if (key <= 0 || key >= platform::KEY_COUNT || frames <= 0)
            return;
        std::lock_guard<std::mutex> lock(g_keyMutex);
        for (int i = 0; i < g_heldCount; ++i)
            if (g_heldKeys[i].key == key)
            {
                g_heldKeys[i].frames = std::max(g_heldKeys[i].frames, frames);
                return;
            }
        if (g_heldCount < static_cast<int>(sizeof(g_heldKeys) / sizeof(g_heldKeys[0])))
            g_heldKeys[g_heldCount++] = {key, frames};
    }

    void applyInjectedKeys(bool *keys, int count, bool frameStart)
    {
        std::lock_guard<std::mutex> lock(g_keyMutex);
        for (int i = 0; i < g_heldCount;)
        {
            HeldKey &k = g_heldKeys[i];
            if (k.key < count)
                keys[k.key] = true;
            if (frameStart && --k.frames <= 0)
                k = g_heldKeys[--g_heldCount];
            else
                ++i;
        }
    }

    void noteGameState(int bits) { g_gameState.store(bits, std::memory_order_relaxed); }
    void noteRumble(uint32_t packed) { g_rumble.store(packed, std::memory_order_relaxed); }
}

using namespace vcs_android;

extern "C"
{
    // on: the overlay is shown (off: the pad reads as not there, nothing held); buttons: 1 << platform::GamepadButton;
    // sticks -1..1 (down / right +), triggers 0..1
    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_TouchControls_nativeSetPad(JNIEnv *, jclass, jboolean on, jint buttons,
                                                                                       jfloat lx, jfloat ly, jfloat rx, jfloat ry,
                                                                                       jfloat lt, jfloat rt)
    {
        const float axes[platform::AXIS_COUNT] = {lx, ly, rx, ry, lt, rt};
        for (int a = 0; a < platform::AXIS_COUNT; ++a)
            g_axes[a].store(std::clamp(axes[a], a >= platform::AXIS_LEFT_TRIGGER ? 0.0f : -1.0f, 1.0f), std::memory_order_relaxed);
        g_buttons.store(on ? static_cast<uint32_t>(buttons) & ((1u << platform::GAMEPAD_BUTTON_COUNT) - 1u) : 0u,
                        std::memory_order_relaxed);
        if (g_on.exchange(on != JNI_FALSE, std::memory_order_relaxed) != (on != JNI_FALSE))
            std::fprintf(stderr, "[touch] on-screen controls %s\n", on ? "shown" : "hidden");
    }

    // a camera drag, in mouse counts (the view scales the finger's movement)
    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_TouchControls_nativeLook(JNIEnv *, jclass, jfloat dx, jfloat dy)
    {
        std::lock_guard<std::mutex> lock(g_lookMutex);
        g_lookX += dx;
        g_lookY += dy;
    }

    JNIEXPORT jboolean JNICALL Java_io_github_batinium_vcsrecomp_TouchControls_nativeLookWanted(JNIEnv *, jclass)
    {
        return g_lookWanted.load(std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
    }

    JNIEXPORT jboolean JNICALL Java_io_github_batinium_vcsrecomp_TouchControls_nativeRealGamepad(JNIEnv *, jclass)
    {
        return g_realGamepad.load(std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
    }

    // the settings button: opens the game's settings menu, or closes it when open
    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_TouchControls_nativeToggleMenu(JNIEnv *, jclass)
    {
        g_menuToggle.store(true, std::memory_order_relaxed);
    }

    // the trainer button: opens the trainer, or closes it when open
    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_TouchControls_nativeToggleTrainer(JNIEnv *, jclass)
    {
        g_trainerToggle.store(true, std::memory_order_relaxed);
    }

    // the game's state for the controls' layout (GameStateBits): in a vehicle, aiming, a menu open...
    JNIEXPORT jint JNICALL Java_io_github_batinium_vcsrecomp_TouchControls_nativeGameState(JNIEnv *, jclass)
    {
        return g_gameState.load(std::memory_order_relaxed);
    }

    // the game's vibration now: low << 16 | high (0..65535 each)
    JNIEXPORT jint JNICALL Java_io_github_batinium_vcsrecomp_Rumble_nativeLevel(JNIEnv *, jclass)
    {
        return static_cast<jint>(g_rumble.load(std::memory_order_relaxed));
    }
}
