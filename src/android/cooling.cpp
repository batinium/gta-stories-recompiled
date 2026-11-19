// [android] See cooling.h. The step-down changes the frame rate cap the way the setting does (vcs::setFrameCap, live)
// without saving it: vcs.ini keeps the player's cap, which comes back when the phone has cooled down (or at the next
// start). A cap the player changes while it is held wins, and the step-down then waits five minutes before it acts again.
#include "android/cooling.h"
#include "android/power.h"

#include "game/vcs_framerate.h"
#include "settings/settings.h"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace vcs_android
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr int kStatusModerate = 2;  // ATHERMAL_STATUS_MODERATE
        constexpr float kHotHeadroom = 0.95f; // the 10 s forecast this close to throttling (1.0) counts as hot
        constexpr float kCoolHeadroom = 0.80f;
        constexpr auto kCoolFor = std::chrono::seconds(60);   // cool this long before the cap comes back
        constexpr auto kPlayerWins = std::chrono::minutes(5); // after the player changed the cap while it was held

        bool g_held = false;
        Clock::time_point g_lastCheck{}, g_coolSince{}, g_quietUntil{};

        const char *statusText(int s)
        {
            static const char *const names[] = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
            return s >= 0 && s < 7 ? names[s] : "unknown";
        }

        void release(const char *why)
        {
            g_held = false;
            setCooling(false);
            setPreferPowerEfficiency(false);
            const int cap = settings::value("display.fps");
            if (vcs::frameCap() == 30)
                vcs::setFrameCap(cap);
            std::fprintf(stderr, "[power] cooling off (%s): frame rate cap %d again\n", why, cap);
        }
    }

    void registerPowerSettings()
    {
        using settings::Type;
        settings::Spec s;
        s.key = "android.hot";
        s.page = "Display";
        s.section = "Phone heat";
        s.label = "When the phone gets hot";
        s.help = "Drop to 30 fps: when the phone reports moderate heat (or is about to throttle), the game runs at 30 fps "
                 "until it has been cool for a minute, then at your frame rate cap again (the stats overlay shows it). "
                 "Keeps the phone from throttling hard. Keep: the cap stays, the phone slows down by itself.";
        s.type = Type::Choice;
        s.choices = {{"Keep the frame rate", 0}, {"Drop to 30 fps", 1}};
        s.defaultValue = 1;
        s.env = "VCS_COOLING";
        s.onChange = [](int v) {
            if (v == 0 && g_held)
                release("setting off");
        };
        settings::add(s);

        s = {};
        s.key = "android.adpf";
        s.page = "Display";
        s.section = "Phone heat";
        s.label = "Performance hints";
        s.help = "Tells Android how long each frame's work takes against the frame time (ADPF), so it can pick lower CPU "
                 "clocks when there is time to spare. Off: Android's own guess from the load.";
        s.type = Type::Toggle;
        s.defaultValue = 1;
        s.env = "VCS_ADPF";
        s.onChange = [](int v) { setPerformanceHints(v != 0); };
        settings::add(s);
    }

    void coolingHostFrame()
    {
        const auto now = Clock::now();
        if (now - g_lastCheck < std::chrono::seconds(1))
            return;
        g_lastCheck = now;
        const int status = thermalStatus();
        const float headroom = thermalHeadroom();
        const bool known = !std::isnan(headroom);
        if (g_held)
        {
            if (vcs::frameCap() != 30) // the player chose a cap meanwhile: theirs
            {
                g_held = false;
                setCooling(false);
                setPreferPowerEfficiency(false);
                g_quietUntil = now + kPlayerWins;
                std::fprintf(stderr, "[power] cooling off: the frame rate cap was changed (%d)\n", vcs::frameCap());
                return;
            }
            if (settings::value("android.hot") == 0)
                return release("setting off");
            const bool cool = status >= 0 && status < kStatusModerate && (!known || headroom < kCoolHeadroom);
            if (!cool)
                g_coolSince = {};
            else if (g_coolSince == Clock::time_point{})
                g_coolSince = now;
            else if (now - g_coolSince >= kCoolFor)
                release("cool for a minute");
            return;
        }
        if (settings::value("android.hot") == 0 || now < g_quietUntil)
            return;
        const bool hot = status >= kStatusModerate || (known && headroom >= kHotHeadroom);
        if (!hot || vcs::frameCap() == 30)
            return;
        g_held = true;
        g_coolSince = {};
        setCooling(true);
        setPreferPowerEfficiency(true);
        std::fprintf(stderr, "[power] cooling on: thermal %s, headroom %.2f - frame rate capped to 30 (was %d)\n", statusText(status),
                     headroom, vcs::frameCap());
        vcs::setFrameCap(30);
    }
}
