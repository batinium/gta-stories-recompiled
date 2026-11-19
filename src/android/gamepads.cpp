// [android] The controllers the game may read, and the one it does (gamepads.h; docs/ANDROID.md "Controllers").
// Main thread: the SDL events and activeGamepad(). Android's UI thread: the JNI calls (Gamepads.java). One lock.
#include "android/gamepads.h"

#include <SDL3/SDL.h>
#include <jni.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace vcs_android
{
    namespace
    {
        struct Pad
        {
            SDL_JoystickID id = 0;
            SDL_Gamepad *gamepad = nullptr; // null when ignored
            std::string name, ignored;      // ignored: why ("" = a controller)
            uint16_t vendor = 0, product = 0;
            uint64_t addedNs = 0, inputNs = 0; // the last press / stick push (Auto picks the latest)
        };

        std::mutex g_mutex;
        std::vector<Pad> g_pads;
        SDL_JoystickID g_active = 0;
        int g_choice = PAD_AUTO;
        int g_chosenVendor = 0, g_chosenProduct = 0;
        std::string g_chosenName;

        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        bool endsWith(const std::string &s, const char *tail)
        {
            const size_t n = std::char_traits<char>::length(tail);
            return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
        }

        // Devices Android reports with a joystick / D-pad / gamepad source that are not a controller to play with: a
        // pad's motion sensors, touchpad or keyboard node (SDL drops some of these names itself), the phone's own key
        // and sensor devices, a headset's media keys. Names are lower case here.
        std::string ignoreReason(const std::string &name)
        {
            const std::string n = lower(name);
            static const char *kTails[] = {" motion sensors", " touchpad", " imu", " consumer control", " system control",
                                           " keyboard", " mouse"};
            for (const char *t : kTails)
                if (endsWith(n, t))
                    return std::string("not a gamepad (") + (t + 1) + ")";
            static const char *kParts[] = {"uinput-", "gpio", "fingerprint", "-keys", "_keys", "avrcp", "headset"};
            for (const char *p : kParts)
                if (n.find(p) != std::string::npos)
                    return "the phone's or an accessory's keys";
            return {};
        }

        bool sameGroup(const Pad &a, const Pad &b)
        {
            return a.vendor == b.vendor && a.product == b.product && a.name == b.name;
        }

        bool matchesChoice(const Pad &p)
        {
            if (g_chosenVendor || g_chosenProduct)
                return p.vendor == g_chosenVendor && p.product == g_chosenProduct;
            return !g_chosenName.empty() && p.name == g_chosenName;
        }

        // Locked: the pad to use now
        SDL_JoystickID choose()
        {
            if (g_choice == PAD_TOUCH_ONLY)
                return 0;
            const Pad *best = nullptr;
            bool chosenThere = false;
            if (g_choice == PAD_CHOSEN)
                for (const Pad &p : g_pads)
                    if (p.gamepad && matchesChoice(p))
                        chosenThere = true;
            for (const Pad &p : g_pads)
            {
                if (!p.gamepad || (chosenThere && !matchesChoice(p)))
                    continue;
                // the most recent input; before any, the first connected
                if (!best || p.inputNs > best->inputNs || (p.inputNs == best->inputNs && p.addedNs < best->addedNs))
                    best = &p;
            }
            return best ? best->id : 0;
        }

        void added(SDL_JoystickID id)
        {
            for (const Pad &p : g_pads)
                if (p.id == id)
                    return;
            Pad p;
            p.id = id;
            const char *name = SDL_GetGamepadNameForID(id);
            p.name = name ? name : "";
            p.vendor = SDL_GetGamepadVendorForID(id);
            p.product = SDL_GetGamepadProductForID(id);
            p.addedNs = SDL_GetTicksNS();
            p.ignored = ignoreReason(p.name);
            int axes = -1, hats = -1, buttons = -1;
            if (p.ignored.empty())
            {
                p.gamepad = SDL_OpenGamepad(id);
                if (!p.gamepad)
                    p.ignored = std::string("can't open: ") + SDL_GetError();
                else if (SDL_Joystick *j = SDL_GetGamepadJoystick(p.gamepad))
                {
                    // for the log (SDL itself drops key-only devices: gamepadsInit; a hat is a D-pad on Android)
                    axes = SDL_GetNumJoystickAxes(j);
                    hats = SDL_GetNumJoystickHats(j);
                    buttons = SDL_GetNumJoystickButtons(j);
                }
            }
            // the same physical pad seen twice: said in the log, grouped in the list, both read (Auto takes the one used)
            int twins = 0;
            for (const Pad &o : g_pads)
                if (sameGroup(o, p))
                    ++twins;
            const char *serial = p.gamepad ? SDL_GetGamepadSerial(p.gamepad) : nullptr;
            const char *path = SDL_GetGamepadPathForID(id);
            std::fprintf(stderr, "[gamepad] + %u \"%s\" %04x:%04x type %d axes %d hats %d buttons %d serial %s path %s%s -> %s\n",
                         static_cast<unsigned>(id), p.name.c_str(), p.vendor, p.product,
                         static_cast<int>(SDL_GetGamepadTypeForID(id)), axes, hats, buttons, serial ? serial : "-",
                         path ? path : "-", twins ? " (same VID/PID and name as one already here)" : "",
                         p.ignored.empty() ? "used" : ("ignored: " + p.ignored).c_str());
            g_pads.push_back(p);
        }

        void removed(SDL_JoystickID id)
        {
            for (size_t i = 0; i < g_pads.size(); ++i)
                if (g_pads[i].id == id)
                {
                    std::fprintf(stderr, "[gamepad] - %u \"%s\"\n", static_cast<unsigned>(id), g_pads[i].name.c_str());
                    if (g_pads[i].gamepad)
                        SDL_CloseGamepad(g_pads[i].gamepad);
                    g_pads.erase(g_pads.begin() + static_cast<long>(i));
                    return;
                }
        }

        void input(SDL_JoystickID id)
        {
            for (Pad &p : g_pads)
                if (p.id == id)
                    p.inputNs = SDL_GetTicksNS();
        }
    }

    void gamepadsInit()
    {
        // SDL's Android backend takes every device with a D-pad source as a joystick and makes each a gamepad (its
        // automatic Android mapping): a device with keys only (fewer than two axes, no hat) is a keyboard instead
        SDL_SetHint(SDL_HINT_TV_REMOTE_AS_JOYSTICK, "0");
        // HIDAPI stays off, as SDL has it on Android (it would ask for USB permission and take the device exclusively;
        // SDL hides the Android device of a pad HIDAPI drives, so it is not a source of doubles either way)
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
    }

    bool gamepadEvent(const SDL_Event &e)
    {
        switch (e.type)
        {
        case SDL_EVENT_GAMEPAD_ADDED:
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            added(e.gdevice.which);
            return true;
        }
        case SDL_EVENT_GAMEPAD_REMOVED:
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            removed(e.gdevice.which);
            return true;
        }
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            input(e.gbutton.which);
            return false;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (e.gaxis.value > 16000 || e.gaxis.value < -16000) // a stick pushed or a trigger pressed, not drift
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                input(e.gaxis.which);
            }
            return false;
        default:
            return false;
        }
    }

    SDL_Gamepad *activeGamepad()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const SDL_JoystickID id = choose();
        if (id != g_active)
        {
            for (const Pad &p : g_pads)
                if (p.id == id)
                    std::fprintf(stderr, "[gamepad] using %u \"%s\"\n", static_cast<unsigned>(id), p.name.c_str());
            if (!id)
                std::fprintf(stderr, "[gamepad] using none (%s)\n", g_choice == PAD_TOUCH_ONLY ? "touch only" : "no controller");
            g_active = id;
        }
        for (const Pad &p : g_pads)
            if (p.id == id)
                return p.gamepad;
        return nullptr;
    }

    void gamepadsShutdown()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (Pad &p : g_pads)
            if (p.gamepad)
                SDL_CloseGamepad(p.gamepad);
        g_pads.clear();
        g_active = 0;
    }
}

using namespace vcs_android;

extern "C"
{
    // The choice (PadChoice) and, for PAD_CHOSEN, the pad: VID / PID (0 0: by name) and its name
    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_Gamepads_nativeSetChoice(JNIEnv *env, jclass, jint choice, jint vendor,
                                                                                    jint product, jstring jname)
    {
        std::string name;
        if (jname)
        {
            const char *c = env->GetStringUTFChars(jname, nullptr);
            name = c ? c : "";
            if (c)
                env->ReleaseStringUTFChars(jname, c);
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_choice = choice >= PAD_AUTO && choice <= PAD_TOUCH_ONLY ? choice : PAD_AUTO;
        g_chosenVendor = vendor;
        g_chosenProduct = product;
        g_chosenName = name;
        std::fprintf(stderr, "[gamepad] choice %d (%04x:%04x \"%s\")\n", g_choice, vendor & 0xFFFF, product & 0xFFFF, name.c_str());
    }

    // The controllers SDL reports, one line each, the same pad's devices as one: name TAB vid TAB pid TAB state
    // (0 connected, 1 in use, 2 ignored) TAB devices TAB seconds since its last input (-1: none yet) TAB why ignored
    JNIEXPORT jstring JNICALL Java_io_github_batinium_vcsrecomp_Gamepads_nativeList(JNIEnv *env, jclass)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const uint64_t now = SDL_WasInit(SDL_INIT_GAMEPAD) ? SDL_GetTicksNS() : 0;
        std::string out;
        std::vector<bool> done(g_pads.size(), false);
        for (size_t i = 0; i < g_pads.size(); ++i)
        {
            if (done[i])
                continue;
            const Pad &p = g_pads[i];
            int devices = 0, state = p.gamepad ? 0 : 2;
            uint64_t last = 0;
            for (size_t k = i; k < g_pads.size(); ++k)
                if (!done[k] && sameGroup(g_pads[k], p) && (g_pads[k].gamepad != nullptr) == (p.gamepad != nullptr))
                {
                    done[k] = true;
                    ++devices;
                    last = std::max(last, g_pads[k].inputNs);
                    if (g_pads[k].gamepad && g_pads[k].id == g_active)
                        state = 1;
                }
            const long ago = last && now > last ? static_cast<long>((now - last) / 1000000000ull) : -1;
            out += p.name + "\t" + std::to_string(p.vendor) + "\t" + std::to_string(p.product) + "\t" + std::to_string(state) +
                   "\t" + std::to_string(devices) + "\t" + std::to_string(ago) + "\t" + p.ignored + "\n";
        }
        return env->NewStringUTF(out.c_str());
    }
}
