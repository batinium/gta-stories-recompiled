// [android] See diagnostics.h. One thread of its own, a few atomics read from the game's threads; the only output is
// fprintf(stderr), which android_log.cpp copies to logcat and files/logs/game.log. No allocation in the steady state
// (the heartbeat line is formatted on the stack).
#include "android/diagnostics.h"
#include "android/stats.h"

#include <jni.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

namespace vcs_android
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        int64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }

        // pushed by Diag.java (-1: not known yet)
        std::atomic<int> g_thermal{-1}, g_battPct{-1}, g_battTenthC{-1000}, g_memFreeMb{-1};
        std::atomic<bool> g_foreground{true};
        std::atomic<bool> g_started{false};

        constexpr int64_t kHbNs = 10ll * 1000000000;
        constexpr int64_t kStallNs = 8ll * 1000000000;

        const char *thermalName(int s)
        {
            static const char *const names[] = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
            return s >= 0 && s <= 6 ? names[s] : "?";
        }

        // a small file (/proc/self/task/N/comm ...) into buf, trailing newline / spaces cut; false when unreadable
        bool slurp(const char *path, char *buf, size_t size)
        {
            const int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                return false;
            const ssize_t n = read(fd, buf, size - 1);
            close(fd);
            if (n <= 0)
                return false;
            buf[n] = '\0';
            size_t len = static_cast<size_t>(n);
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == ' '))
                buf[--len] = '\0';
            return true;
        }

        // the state letter of /proc/self/task/N/stat: the first character after the ") " that closes the (comm) field
        char stateLetter(const char *stat)
        {
            const char *close = std::strrchr(stat, ')');
            return close && close[1] == ' ' && close[2] ? close[2] : '?';
        }

        const char *stateText(char c)
        {
            switch (c)
            {
            case 'R': return "running";
            case 'S': return "sleeping";
            case 'D': return "disk wait";
            case 'T': case 't': return "stopped";
            case 'Z': return "zombie";
            default: return "?";
            }
        }

        // every native thread: name, state letter, what the kernel says it waits in (often empty for apps)
        void dumpThreads()
        {
            DIR *d = opendir("/proc/self/task");
            if (!d)
            {
                std::fprintf(stderr, "[watchdog]   (cannot read /proc/self/task)\n");
                return;
            }
            int count = 0;
            while (const dirent *e = readdir(d))
            {
                if (e->d_name[0] < '0' || e->d_name[0] > '9')
                    continue;
                char path[96], comm[64] = "?", stat[512] = "", wchan[96] = "";
                std::snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
                slurp(path, comm, sizeof(comm));
                std::snprintf(path, sizeof(path), "/proc/self/task/%s/stat", e->d_name);
                const bool haveStat = slurp(path, stat, sizeof(stat));
                std::snprintf(path, sizeof(path), "/proc/self/task/%s/wchan", e->d_name);
                slurp(path, wchan, sizeof(wchan));
                const char s = haveStat ? stateLetter(stat) : '?';
                std::fprintf(stderr, "[watchdog]   tid %s %s: %c (%s) wchan=%s\n", e->d_name, comm, s, stateText(s),
                             wchan[0] && std::strcmp(wchan, "0") != 0 ? wchan : "-");
                ++count;
            }
            closedir(d);
            std::fprintf(stderr, "[watchdog] %d threads\n", count);
        }

        void run(bool watchdog)
        {
            const int64_t start = nowNs();
            int64_t nextHb = start + kHbNs, hbAt = start;
            uint64_t hbFrames = hostFrameTotal(), lastSeen = hbFrames;
            int64_t advancedAt = start;
            bool stallLogged = false, wasForeground = true;
            char lastHb[320] = "(none yet)";
            const char *game = std::getenv("VCS_DIAG_GAME");
            const char *drv = std::getenv("VCS_GPU_DRIVER");
            const char *drivers = std::getenv("VCS_GPU_DRIVERS");
            const bool custom = drv && *drv && !(drivers && std::strcmp(drivers, "0") == 0);
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const int64_t now = nowNs();
                const uint64_t frames = hostFrameTotal();
                const bool fg = g_foreground.load(std::memory_order_relaxed);
                if (frames != lastSeen || fg != wasForeground) // a frame, or the app came to / left the foreground: restart the clock
                {
                    lastSeen = frames;
                    advancedAt = now;
                    stallLogged = false;
                }
                wasForeground = fg;
                if (now >= nextHb)
                {
                    const double s = (now - hbAt) / 1e9;
                    const int64_t gap = takeMaxFrameGapNs();
                    const int bt = g_battTenthC.load(std::memory_order_relaxed);
                    const int bp = g_battPct.load(std::memory_order_relaxed), mf = g_memFreeMb.load(std::memory_order_relaxed);
                    char batt[32] = "?/?", mem[16] = "?";
                    if (bp >= 0 && bt > -1000)
                        std::snprintf(batt, sizeof(batt), "%d/%.1f", bp, bt / 10.0);
                    else if (bp >= 0)
                        std::snprintf(batt, sizeof(batt), "%d/?", bp);
                    if (mf >= 0)
                        std::snprintf(mem, sizeof(mem), "%d", mf);
                    std::snprintf(lastHb, sizeof(lastHb),
                                  "[hb] t=%lld frames=%llu fps=%.1f frame_ms_max=%.1f thermal=%s battery=%s mem_free=%s game=%s driver=%s%s",
                                  static_cast<long long>((now - start) / 1000000000), static_cast<unsigned long long>(frames),
                                  s > 0 ? (frames - hbFrames) / s : 0.0, gap / 1e6, thermalName(g_thermal.load(std::memory_order_relaxed)),
                                  batt, mem, game && *game ? game : "?", custom ? drv : "system", fg ? "" : " bg=1");
                    std::fprintf(stderr, "%s\n", lastHb);
                    hbFrames = frames;
                    hbAt = now;
                    nextHb += kHbNs;
                }
                if (watchdog && fg && !stallLogged && now - advancedAt >= kStallNs)
                {
                    stallLogged = true;
                    std::fprintf(stderr, "[watchdog] no frame for %lld s (frames so far %llu%s; last hb: %s)\n",
                                 static_cast<long long>((now - advancedAt) / 1000000000), static_cast<unsigned long long>(frames),
                                 frames == 0 ? ", none presented yet: still starting?" : "", lastHb);
                    dumpThreads();
                }
            }
        }
    }

    void startDiagnostics()
    {
        if (g_started.exchange(true))
            return;
        const char *w = std::getenv("VCS_WATCHDOG");
        const bool watchdog = !(w && std::strcmp(w, "0") == 0);
        std::fprintf(stderr, "[android] diagnostics: heartbeat every 10 s, watchdog %s\n", watchdog ? "on (8 s)" : "off (VCS_WATCHDOG=0)");
        std::thread(run, watchdog).detach();
    }
}

extern "C"
{
    // Diag.java: thermal status (-1 unknown), battery percent (-1), battery temperature in tenths of a degree C (-1000),
    // free memory MB (-1), the app is in the foreground
    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_Diag_nativePush(JNIEnv *, jclass, jint thermal, jint battPct, jint battTenthC,
                                                                            jint memFreeMb, jboolean foreground)
    {
        using namespace vcs_android;
        g_thermal.store(thermal, std::memory_order_relaxed);
        g_battPct.store(battPct, std::memory_order_relaxed);
        g_battTenthC.store(battTenthC, std::memory_order_relaxed);
        g_memFreeMb.store(memFreeMb, std::memory_order_relaxed);
        g_foreground.store(foreground != JNI_FALSE, std::memory_order_relaxed);
    }
}
