// [android] The side panel's JNI bridge in the game (side_panel.h). The Java panel calls from Android's UI thread; the
// settings registry is changed on the main thread only (settings.h), so changes and actions are queued here and run in
// panelHostFrame. Reading is safe from any thread: the specs are fixed once registered (before the first host frame,
// which is when the panel calls itself ready) and value() is any-thread.
#include "android/side_panel.h"
#include "android/touch_controls.h"

#include "game/vcs_aim.h"
#include "game/vcs_framerate.h"
#include "game/vcs_hud.h"
#include "game/vcs_input.h"
#include "game/vcs_frontend.h"
#include "game/vcs_trainer.h"
#include "game/game_table.h"   // [lcs] game::isLcs
#include "game/lcs_aim.h"      // [lcs] the controls' state on LCS
#include "game/lcs_driveby.h"  // [lcs] its drive-by look counts as aiming
#include "game/lcs_frontend.h"
#include "game/trainer_select.h" // [lcs] the trainer (in a vehicle, free camera) and saves of the game that runs
#include "platform/platform.h"
#include "runtime/ps2_control.h"
#include "settings/settings.h"
#include "ui/settings_menu.h"
#include "ui/trainer_menu.h"

#include <jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace vcs_android
{
    namespace
    {
        std::atomic<bool> g_ready{false};
        std::mutex g_queueMutex;
        std::vector<std::function<void()>> g_queue;
        std::mutex g_shotMutex;
        std::vector<std::string> g_shots; // finished screenshots: a path, or "" for a failed one
        std::atomic<bool> g_openRequest{false};
        bool g_guideWas = false;
        bool g_chordFired = false;
        std::chrono::steady_clock::time_point g_chordSince{};
        bool g_chordHeld = false;
        bool g_touchChordWas = false;
        std::atomic<bool> g_touchToggle{false}; // Back + the touch shortcut: TouchControls shows / hides itself
        bool g_ffChordWas = false;
        std::atomic<float> g_ffSpeed{2.0f}; // the fast forward shortcut's speed (TouchControls: the panel's setting)

        void enqueue(std::function<void()> f)
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            g_queue.push_back(std::move(f));
        }

        const settings::Spec *find(const std::string &key)
        {
            for (const settings::Spec &s : settings::all())
                if (s.key == key)
                    return &s;
            return nullptr;
        }

        std::string jstr(JNIEnv *env, jstring s)
        {
            if (!s)
                return {};
            const char *c = env->GetStringUTFChars(s, nullptr);
            std::string out = c ? c : "";
            if (c)
                env->ReleaseStringUTFChars(s, c);
            return out;
        }

        std::string oneLine(std::string s)
        {
            std::replace(s.begin(), s.end(), '\n', ' ');
            return s;
        }

        // The controller's bindings (the Gamepad page's pad.<id> choices: a GamepadButton, 20 LT, 21 RT, -1 none) as the
        // overlay's map (touch_controls.h setTouchBindings): each PS2 button's default source -> the source bound now.
        bool padBinding(const settings::Spec &s)
        {
            return s.type == settings::Type::Choice && s.key.rfind("pad.", 0) == 0 && !s.choices.empty() &&
                   s.choices.back().value == -1; // the binding choices end with None
        }

        int sourceBit(int source) // a binding's source as a mask bit index, or -1
        {
            if (source >= 0 && source < platform::GAMEPAD_BUTTON_COUNT)
                return source;
            return source == 20 ? 14 : source == 21 ? 15 : -1;
        }

        void publishTouchBindings()
        {
            uint32_t map[16];
            for (int i = 0; i < 16; ++i)
                map[i] = 1u << i;
            // the settings menu and the trainer read the pad as it is (menu navigation): no translation meanwhile
            if (!ui::settingsMenuOpen() && !ui::trainerMenuOpen())
            {
                bool seen[16] = {};
                for (const settings::Spec &s : settings::all())
                {
                    if (!padBinding(s))
                        continue;
                    const int from = sourceBit(s.defaultValue), to = sourceBit(settings::value(s.key));
                    if (from < 0)
                        continue;
                    if (!seen[from])
                        map[from] = 0;
                    seen[from] = true;
                    if (to >= 0)
                        map[from] |= 1u << to;
                }
            }
            setTouchBindings(map);
        }
    }

    namespace
    {
        // [lcs] The settings this game's run locks (settings::lockForRun: lcs_settings.cpp's VCS-only rows), for the
        // launcher's pre-launch rows (VcsIni.lockedForGame): <data>/locked_settings.txt, "key TAB note" a line, written
        // once a start into the game's own data folder (VCS_DATA_DIR: files/userdata, files/userdata-lcs), so each
        // game's list is its own and the one source stays the runtime's
        void writeLockedSettings()
        {
            const char *data = std::getenv("VCS_DATA_DIR");
            if (!data || !*data)
                return;
            std::string out;
            for (const std::string &key : settings::lockedForRun())
                if (const settings::Spec *s = find(key))
                    out += key + "\t" + oneLine(s->lockedNote) + "\n";
            const std::filesystem::path path = std::filesystem::path(data) / "locked_settings.txt";
            const std::filesystem::path tmp = path.string() + ".tmp";
            if (FILE *f = std::fopen(tmp.string().c_str(), "wb"))
            {
                const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
                if (std::fclose(f) == 0 && ok)
                {
                    std::error_code ec;
                    std::filesystem::rename(tmp, path, ec);
                    return;
                }
            }
            std::fprintf(stderr, "[panel] could not write %s\n", path.string().c_str());
        }
    }

    void panelHostFrame()
    {
        if (!g_ready.exchange(true, std::memory_order_relaxed))
            writeLockedSettings();
        std::vector<std::function<void()>> work;
        {
            std::lock_guard<std::mutex> lock(g_queueMutex);
            work.swap(g_queue);
        }
        for (auto &f : work)
            f();

        namespace tr = game::trainer; // [lcs] VCS's or LCS's trainer (in a vehicle, the free camera)
        int state = STATE_GAME;
        if (tr::ready() && tr::inVehicle())
            state |= STATE_IN_VEHICLE;
        else if (tr::ready() && tr::nearVehicle())
            state |= STATE_NEAR_VEHICLE;
        // [lcs] LCS's free aim and pause menu (vcs::aim / vcs::frontend read VCS's memory)
        const bool lcs = game::isLcs();
        // [lcs] and LCS's drive-by look (L2 / R2 with a drive-by weapon: the mouse / gyro aim it, lcs_driveby.h); VCS's
        // drive-by is not aimed (the game's fixed side shot), so it is not "aiming" there
        if ((lcs ? lcs::aim::aiming() || lcs::driveby::aiming() : vcs::aim::aiming()) ||
            platform::gamepadButton(platform::GAMEPAD_RIGHT_SHOULDER))
            state |= STATE_AIMING;
        if (ui::settingsMenuOpen())
            state |= STATE_SETTINGS_MENU;
        if (ui::trainerMenuOpen())
            state |= STATE_TRAINER;
        if (tr::toggle(tr::FreeCamera))
            state |= STATE_FREE_CAMERA;
        if (lcs ? lcs::frontend::open() : vcs::frontend::open())
            state |= STATE_PAUSE_MENU;
        noteGameState(state);
        publishTouchBindings();

        // a real controller opens the panel: Home / Guide, or Select + Start held 0.7 s (pressed, the pair opens the
        // settings menu at once, as on the PC: that is closed again when the hold opens the panel)
        float axes[platform::AXIS_COUNT];
        const uint32_t pad = realPad(axes);
        const bool guide = (pad & kGuideBit) != 0;
        if (guide && !g_guideWas)
            g_openRequest.store(true, std::memory_order_relaxed);
        g_guideWas = guide;
        // the touch controls' shortcut (Gamepad > Shortcuts): Back + its button, on the press
        const int touchButton = vcs::input::shortcutButton(vcs::input::Shortcut::TouchControls);
        const uint32_t touchChord = touchButton >= 0 ? (1u << platform::GAMEPAD_BACK | 1u << touchButton) : 0u;
        const bool touchDown = touchChord && (pad & touchChord) == touchChord;
        if (touchDown && !g_touchChordWas)
            g_touchToggle.store(true, std::memory_order_relaxed);
        g_touchChordWas = touchDown;
        // fast forward's shortcut: on / off at the press
        const int ffButton = vcs::input::shortcutButton(vcs::input::Shortcut::FastForward);
        const uint32_t ffChord = ffButton >= 0 ? (1u << platform::GAMEPAD_BACK | 1u << ffButton) : 0u;
        const bool ffDown = ffChord && (pad & ffChord) == ffChord;
        if (ffDown && !g_ffChordWas)
            vcs::setFastForward(vcs::fastForward() > 1.0f ? 1.0f : g_ffSpeed.load(std::memory_order_relaxed));
        g_ffChordWas = ffDown;
        const int panelButton = vcs::input::shortcutButton(vcs::input::Shortcut::Panel);
        const uint32_t chord = panelButton >= 0 ? (1u << platform::GAMEPAD_BACK | 1u << panelButton) : 0u;
        const bool held = chord && (pad & chord) == chord;
        const auto now = std::chrono::steady_clock::now();
        if (held && !g_chordHeld)
        {
            g_chordSince = now;
            g_chordFired = false;
        }
        g_chordHeld = held;
        if (held && !g_chordFired && now - g_chordSince > std::chrono::milliseconds(700))
        {
            g_chordFired = true;
            ui::closeSettingsMenu();
            g_openRequest.store(true, std::memory_order_relaxed);
        }
    }
}

using namespace vcs_android;

extern "C"
{
    // the game's settings are registered and its frames run: the panel's settings rows can be shown
    JNIEXPORT jboolean JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativeReady(JNIEnv *, jclass)
    {
        return g_ready.load(std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
    }

    // One setting, as lines (SidePanel.Setting parses them), or null when the registry has no such key:
    // type, apply (1: next start), value, default, min, max, flags (1 overridden by its environment variable, 2 changed
    // since the start, 4 locked by another setting), locked value, format, label, help, button, locked note, then one
    // line per choice: value TAB label.
    JNIEXPORT jstring JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativeSettingInfo(JNIEnv *env, jclass, jstring jkey)
    {
        if (!g_ready.load(std::memory_order_relaxed))
            return nullptr;
        const std::string key = jstr(env, jkey);
        const settings::Spec *s = find(key);
        if (!s || s->hidden)
            return nullptr;
        const int flags = (settings::overridden(key) ? 1 : 0) | (settings::changedSinceStart(key) ? 2 : 0) |
                          (s->locked && s->locked() ? 4 : 0);
        std::string out;
        auto line = [&](const std::string &v) { out += oneLine(v) + "\n"; };
        line(std::to_string(static_cast<int>(s->type)));
        line(s->apply == settings::Apply::Restart ? "1" : "0");
        line(std::to_string(s->type == settings::Type::Action ? 0 : settings::value(key)));
        line(std::to_string(s->defaultValue));
        line(std::to_string(s->min));
        line(std::to_string(s->max));
        line(std::to_string(flags));
        line(std::to_string(s->lockedValue));
        line(s->format);
        line(s->label);
        line(s->help);
        line(s->button);
        line(s->lockedNote);
        for (const settings::Choice &c : s->choices)
            line(std::to_string(c.value) + "\t" + c.label);
        return env->NewStringUTF(out.c_str());
    }

    // a setting changed (validated, saved to vcs.ini and applied by the registry on the main thread)
    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativeSettingSet(JNIEnv *env, jclass, jstring jkey, jint v)
    {
        std::string key = jstr(env, jkey);
        enqueue([key, v] {
            if (!settings::set(key, v))
                std::fprintf(stderr, "[panel] setting %s = %d refused\n", key.c_str(), static_cast<int>(v));
        });
    }

    // an Action setting's button (presets, resets)
    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativeSettingPress(JNIEnv *env, jclass, jstring jkey)
    {
        std::string key = jstr(env, jkey);
        enqueue([key] {
            const settings::Spec *s = find(key);
            if (s && s->type == settings::Type::Action && s->onChange)
            {
                std::fprintf(stderr, "[panel] %s pressed\n", key.c_str());
                s->onChange(0);
            }
        });
    }

    // the live values of settings something else changes too (a hotkey: F3's overlay), taken before the panel shows them
    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativeRefresh(JNIEnv *, jclass)
    {
        enqueue([] { settings::refresh(); });
    }

    // 1: the free camera on / off (F6), 2: the game's pause menu (Escape, as Back was), 3: the perf CSV on / off (F4)
    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativeRequest(JNIEnv *, jclass, jint what)
    {
        switch (what)
        {
        case 1:
            enqueue([] {
                namespace tr = game::trainer; // [lcs]
                tr::setToggle(tr::FreeCamera, !tr::toggle(tr::FreeCamera));
            });
            break;
        case 2:
            injectKey(platform::KEY_ESCAPE, 4);
            break;
        case 3:
            injectKey(platform::KEY_F4, 2);
            break;
        default:
            break;
        }
    }

    // The Game page's save rows (SidePanel): 0 = "Save game here" (the game's Save menu), 1 = quick save to the quick
    // slot, 2 = quick load from it (LCS only: VCS loads only from its frontend). [lcs] game::saves picks VCS's
    // (vcs_save_anywhere.h) or LCS's (lcs_save.h). The outcome is a trainer notice on screen. Any thread (both queue
    // the request for the game thread).
    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativeSaveAnywhere(JNIEnv *, jclass, jint what)
    {
        namespace sa = game::saves;
        if (what == 2)
            sa::quickLoad(); // nothing on VCS
        else if (what == 1)
            sa::quickSave();
        else
            sa::saveHere();
    }

    // Why saving here is refused now ("" when it is possible, as of the last frame), for the rows' help line
    JNIEXPORT jstring JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativeSaveWhyNot(JNIEnv *env, jclass)
    {
        const std::string why = game::saves::whyNot();
        return env->NewStringUTF(oneLine(why).c_str());
    }

    // A screenshot of the picture as shown (the game's menus included, the Android views over it not) into `dir`, an
    // app folder: SidePanel copies it to Pictures/ through MediaStore once nativeTakeScreenshot hands it over.
    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativeScreenshot(JNIEnv *env, jclass, jstring jdir)
    {
        std::string dir = jstr(env, jdir);
        enqueue([dir] {
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const std::time_t t = std::time(nullptr);
            std::tm local{};
            localtime_r(&t, &local);
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "vcs_%Y-%m-%d_%H-%M-%S", &local);
            std::filesystem::path path = std::filesystem::path(dir) / (std::string(stamp) + ".png");
            for (int n = 2; std::filesystem::exists(path, ec); ++n)
                path = std::filesystem::path(dir) / (std::string(stamp) + "_" + std::to_string(n) + ".png");
            ui::hideScreenshotNotices();
            const std::string p = path.string();
            ps2control::requestScreenshot(p, [p](const std::string &size) {
                std::fprintf(stderr, "[panel] screenshot %s (%s)\n", p.c_str(), size.empty() ? "failed" : size.c_str());
                std::lock_guard<std::mutex> lock(g_shotMutex);
                g_shots.push_back(size.empty() ? std::string() : p);
            });
        });
    }

    // a finished screenshot's path ("" for a failed one), or null when none is waiting
    JNIEXPORT jstring JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativeTakeScreenshot(JNIEnv *env, jclass)
    {
        std::string path;
        {
            std::lock_guard<std::mutex> lock(g_shotMutex);
            if (g_shots.empty())
                return nullptr;
            path = g_shots.front();
            g_shots.erase(g_shots.begin());
        }
        return env->NewStringUTF(path.c_str());
    }

    // The pad for the panel's focus: the real one's and the on-screen controls' buttons (1 << GamepadButton; the
    // overlay's L2 / R2 as their bits 14 / 15) and the sticks (out[0..3]: lx ly rx ry) of either.
    JNIEXPORT jint JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativeNavPad(JNIEnv *env, jclass, jfloatArray out)
    {
        float axes[platform::AXIS_COUNT];
        uint32_t b = realPad(axes) & ~kGuideBit;
        const TouchPad touch = touchPad();
        b |= touch.buttons;
        if (axes[platform::AXIS_LEFT_TRIGGER] > 0.5f || touch.axes[platform::AXIS_LEFT_TRIGGER] > 0.5f)
            b |= 1u << 14;
        if (axes[platform::AXIS_RIGHT_TRIGGER] > 0.5f || touch.axes[platform::AXIS_RIGHT_TRIGGER] > 0.5f)
            b |= 1u << 15;
        if (out && env->GetArrayLength(out) >= 4)
        {
            float v[4];
            for (int a = 0; a < 4; ++a)
                v[a] = std::fabs(touch.axes[a]) > std::fabs(axes[a]) ? touch.axes[a] : axes[a];
            env->SetFloatArrayRegion(out, 0, 4, v);
        }
        return static_cast<jint>(b);
    }

    // The registry's controller bindings (pad.<id> choices, registry order), one key a line, for the Remap screen
    JNIEXPORT jstring JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativePadBindings(JNIEnv *env, jclass)
    {
        if (!g_ready.load(std::memory_order_relaxed))
            return nullptr;
        std::string out;
        for (const settings::Spec &s : settings::all())
            if (padBinding(s) && !s.hidden)
                out += s.key + "\n";
        return env->NewStringUTF(out.c_str());
    }

    // The real controller alone (not the overlay), for the Remap screen's capture: 1 << GamepadButton, 1 << 14 / 15 the
    // left / right trigger pressed
    JNIEXPORT jint JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativeRealPad(JNIEnv *, jclass)
    {
        float axes[platform::AXIS_COUNT];
        uint32_t b = realPad(axes) & ~kGuideBit;
        if (axes[platform::AXIS_LEFT_TRIGGER] > 0.5f)
            b |= 1u << 14;
        if (axes[platform::AXIS_RIGHT_TRIGGER] > 0.5f)
            b |= 1u << 15;
        return static_cast<jint>(b);
    }

    // Home / Guide pressed, or Select + Start held, on a real controller since the last call
    // fast forward (TouchControls' button; 1 = off), its state, the gamepad shortcut's speed
    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_TouchControls_nativeFastForward(JNIEnv *, jclass, jfloat speed)
    {
        vcs::setFastForward(speed);
    }

    JNIEXPORT jfloat JNICALL Java_io_github_batinium_vcsrecomp_TouchControls_nativeFastForwardNow(JNIEnv *, jclass)
    {
        return vcs::fastForward();
    }

    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_TouchControls_nativeSetFastForwardSpeed(JNIEnv *, jclass, jfloat speed)
    {
        g_ffSpeed.store(speed, std::memory_order_relaxed);
        if (vcs::fastForward() > 1.0f) // on: the new speed at once
            vcs::setFastForward(speed);
    }

    // the player's HUD layout from the on-screen layout editor (TouchControls.sendHud)
    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_TouchControls_nativeSetHudParts(JNIEnv *, jclass, jfloat rdx, jfloat rdy,
                                                                                         jfloat rk, jfloat cdx, jfloat cdy, jfloat ck)
    {
        vcs::setHudPart(0, rdx, rdy, rk);
        vcs::setHudPart(1, cdx, cdy, ck);
        std::fprintf(stderr, "[hud] layout: radar %+.0f %+.0f x%.2f, cluster %+.0f %+.0f x%.2f\n", rdx, rdy, rk, cdx, cdy, ck);
    }

    // the touch controls' gamepad shortcut was pressed (Gamepad > Shortcuts), once
    JNIEXPORT jboolean JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativeTakeTouchToggle(JNIEnv *, jclass)
    {
        return g_touchToggle.exchange(false, std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
    }

    JNIEXPORT jboolean JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativeTakeOpenRequest(JNIEnv *, jclass)
    {
        return g_openRequest.exchange(false, std::memory_order_relaxed) ? JNI_TRUE : JNI_FALSE;
    }

    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativeSetOpen(JNIEnv *, jclass, jboolean open)
    {
        setPanelOpen(open != JNI_FALSE);
    }

    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativeSetPaused(JNIEnv *, jclass, jboolean paused)
    {
        setHostPaused(paused != JNI_FALSE);
    }

    // Frame generation switched / changed in the panel: live (the Vulkan present makes its swapchain for it again)
    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativeFrameGen(JNIEnv *, jclass, jboolean on, jint mult, jint flow)
    {
        platform::setFrameGen(on != JNI_FALSE, mult, flow);
    }

    // The player's Lossless.dll (the launcher's copy), named before the switch: one added after the game started is read
    // when frame generation is switched on (framegen_present.inl fgCheckLive)
    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_SidePanel_nativeFrameGenDll(JNIEnv *env, jclass, jstring path)
    {
        platform::setFrameGenDll(jstr(env, path));
    }
}
