// See save_shots.h.
#include "game/save_shots.h"

#include "ps2_runtime.h"
#include "runtime/ps2_control.h"
#include "ui/settings_menu.h"
#include "ui/trainer_menu.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

namespace saveshots
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        constexpr auto kShotEvery = std::chrono::seconds(20);
        constexpr auto kShotSettle = std::chrono::seconds(5); // free play this long first: not a load's fade-in (black)
        constexpr auto kFreshness = std::chrono::milliseconds(400);

        std::atomic<bool> g_freePlay{false};
        std::atomic<int64_t> g_freeStamp{0};  // Clock ticks of the last setFreePlay(true)
        std::atomic<bool> g_wantFresh{false};
        std::atomic<bool> g_freshDone{true};
        std::atomic<bool> g_freshTainted{false}; // a menu was up for the fresh shot: use the free-play one
        std::atomic<bool> g_latestOk{false};     // latest.png was taken in this run
        Clock::time_point g_lastShot{}, g_freeSince{}; // main thread

        bool enabled()
        {
            static const bool on = [] {
                const char *e = std::getenv("VCS_SAVE_SHOTS");
                return !(e && e[0] == '0');
            }();
            return on;
        }

        std::filesystem::path dataDir() { return PS2Runtime::getIoPaths().mcRoot.parent_path(); }
        std::filesystem::path shotsDir() { return dataDir() / "save_shots"; }

        std::string slotFile(int slot)
        {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "slot%d.png", slot);
            return buffer;
        }

        void shoot(const std::string &name, bool latest)
        {
            std::error_code ec;
            std::filesystem::create_directories(shotsDir(), ec);
            const std::filesystem::path to = shotsDir() / name, tmp = shotsDir() / ("part_" + name);
            if (!latest)
                std::filesystem::remove(to, ec);
            ps2control::requestScreenshot(tmp.string(), [to, tmp, latest](const std::string &size) {
                std::error_code e;
                if (size.empty())
                    std::filesystem::remove(tmp, e);
                else
                {
                    std::filesystem::rename(tmp, to, e);
                    if (!e && latest)
                        g_latestOk = true;
                }
                if (!latest)
                    g_freshDone = true;
            });
        }
    }

    void setFreePlay(bool freePlay)
    {
        g_freePlay = freePlay;
        if (freePlay)
            g_freeStamp = Clock::now().time_since_epoch().count();
    }

    void onHostFrame()
    {
        if (!enabled())
            return;
        const auto now = Clock::now();
        const bool menuUp = ui::settingsMenuOpen() || ui::trainerMenuOpen();
        if (g_wantFresh.exchange(false))
        {
            if (menuUp) // the settings / trainer menu would be in the picture
            {
                g_freshTainted = true;
                g_freshDone = true;
            }
            else
            {
                g_freshTainted = false;
                shoot("fresh.png", false);
            }
            return;
        }
        const bool recent = now.time_since_epoch().count() - g_freeStamp.load(std::memory_order_relaxed) <
                            std::chrono::duration_cast<Clock::duration>(kFreshness).count();
        if (!g_freePlay.load(std::memory_order_relaxed) || !recent || menuUp)
        {
            g_freeSince = now;
            return;
        }
        if (now - g_freeSince < kShotSettle || now - g_lastShot < kShotEvery)
            return;
        g_lastShot = now;
        shoot("latest.png", true);
    }

    void requestFresh()
    {
        if (!enabled())
            return;
        g_freshDone = false;
        g_wantFresh = true;
    }

    bool freshDone() { return !enabled() || g_freshDone.load(); }

    void pictureForSlot(int slot, bool fresh)
    {
        if (!enabled())
            return;
        const std::filesystem::path dir = shotsDir();
        const bool haveLatest = g_latestOk.load();
        fresh = fresh && !g_freshTainted.load();
        std::thread([dir, slot, fresh, haveLatest] {
            std::error_code ec;
            const std::filesystem::path to = dir / slotFile(slot);
            const std::filesystem::path from = dir / (fresh ? "fresh.png" : "latest.png");
            if (fresh)
                for (int i = 0; i < 40 && !std::filesystem::exists(from, ec); ++i) // its PNG may still be written
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if ((fresh || haveLatest) && std::filesystem::exists(from, ec))
            {
                std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
                std::fprintf(stderr, "[save-shots] slot %d: picture %s%s\n", slot, from.filename().string().c_str(),
                             ec ? " (copy failed)" : "");
            }
            else
                std::filesystem::remove(to, ec); // none of this save: not an older one's either
        }).detach();
    }

    void removePicture(int slot)
    {
        std::error_code ec;
        if (std::filesystem::remove(shotsDir() / slotFile(slot), ec))
            std::fprintf(stderr, "[save-shots] slot %d deleted: its picture removed\n", slot);
    }
}
