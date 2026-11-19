// [android] See power.h.
#include "android/power.h"

#include <dirent.h>
#include <dlfcn.h>
#include <jni.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace vcs_android
{
    namespace
    {
        // ---- libandroid.so's entry points (Android 13+ for ADPF, 11 / 12 for the thermal ones; 15 for efficiency) ----
        struct HintManager;
        struct HintSession;
        struct ThermalManager;
        struct Api
        {
            HintManager *(*getManager)() = nullptr;
            HintSession *(*createSession)(HintManager *, const int32_t *, size_t, int64_t) = nullptr;
            int (*updateTarget)(HintSession *, int64_t) = nullptr;
            int (*reportActual)(HintSession *, int64_t) = nullptr;
            void (*closeSession)(HintSession *) = nullptr;
            int (*preferEfficiency)(HintSession *, bool) = nullptr; // APerformanceHint_setPreferPowerEfficiency (API 35)
            ThermalManager *(*acquireThermal)() = nullptr;
            int (*thermalStatus)(ThermalManager *) = nullptr;
            float (*thermalHeadroom)(ThermalManager *, int) = nullptr;
            bool loaded = false;
        };

        Api &api()
        {
            static Api a = [] {
                Api r;
                void *h = dlopen("libandroid.so", RTLD_NOW);
                if (!h)
                    return r;
                auto get = [h](auto &fn, const char *name) { fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(h, name)); };
                get(r.getManager, "APerformanceHint_getManager");
                get(r.createSession, "APerformanceHint_createSession");
                get(r.updateTarget, "APerformanceHint_updateTargetWorkDuration");
                get(r.reportActual, "APerformanceHint_reportActualWorkDuration");
                get(r.closeSession, "APerformanceHint_closeSession");
                get(r.preferEfficiency, "APerformanceHint_setPreferPowerEfficiency");
                get(r.acquireThermal, "AThermal_acquireManager");
                get(r.thermalStatus, "AThermal_getCurrentThermalStatus");
                get(r.thermalHeadroom, "AThermal_getThermalHeadroom");
                r.loaded = true;
                return r;
            }();
            return a;
        }

        int64_t monoNs()
        {
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC, &ts);
            return static_cast<int64_t>(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
        }

        // Another thread's CPU time: Linux's per-thread CPU clock id for a tid (MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED)),
        // as pthread_getcpuclockid makes it; -1 when the thread is gone.
        int64_t threadCpuNs(int32_t tid)
        {
            const clockid_t id = static_cast<clockid_t>((~static_cast<uint32_t>(tid) << 3) | 6u);
            timespec ts{};
            if (clock_gettime(id, &ts) != 0)
                return -1;
            return static_cast<int64_t>(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
        }

        // The threads worth a hint: the runtime names them (ThreadNaming); the calling (main) thread is always one.
        constexpr const char *kHintThreads[] = {"GameThread", "RenderWorker", "GsStage", "ChainWalk"}; // ChainWalk: PS2X_CHAIN_ASYNC (LCS)
        constexpr size_t kWanted = 1 + sizeof(kHintThreads) / sizeof(kHintThreads[0]);

        struct Tracked
        {
            int32_t tid;
            std::string name;
            int64_t lastCpu;
        };

        // main thread only
        std::vector<Tracked> g_threads;
        HintSession *g_session = nullptr;
        int64_t g_target = 0, g_lastReport = 0, g_lastScan = 0, g_started = 0;
        bool g_failed = false;
        std::atomic<bool> g_hintsOn{true}, g_preferEfficiency{false};
        bool g_efficiencyApplied = false;
        // the log's numbers (every 30 s): the reported busiest-thread time, frames
        double g_busySumMs = 0.0, g_busyMaxMs = 0.0;
        uint32_t g_busyFrames = 0;
        int64_t g_lastLog = 0;

        ThermalManager *g_thermal = nullptr;
        int64_t g_lastStatusPoll = 0, g_lastHeadroomPoll = 0;
        std::atomic<int> g_status{-1};
        std::atomic<float> g_headroom{NAN};
        std::atomic<bool> g_cooling{false};

        const char *statusName(int s)
        {
            static const char *const names[] = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
            return s >= 0 && s < 7 ? names[s] : "unknown";
        }

        bool envOff(const char *name)
        {
            const char *v = std::getenv(name);
            return v && v[0] == '0';
        }

        void closeSession()
        {
            if (g_session && api().closeSession)
                api().closeSession(g_session);
            g_session = nullptr;
            g_efficiencyApplied = false;
        }

        // /proc/self/task: the named threads not tracked yet (their comm is the name ThreadNaming gave)
        bool scanThreads()
        {
            bool added = false;
            DIR *d = opendir("/proc/self/task");
            if (!d)
                return false;
            while (dirent *e = readdir(d))
            {
                const int32_t tid = static_cast<int32_t>(std::atoi(e->d_name));
                if (tid <= 0 || std::any_of(g_threads.begin(), g_threads.end(), [&](const Tracked &t) { return t.tid == tid; }))
                    continue;
                char path[64], comm[32] = {};
                std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
                const int fd = open(path, O_RDONLY | O_CLOEXEC);
                if (fd < 0)
                    continue;
                const ssize_t n = read(fd, comm, sizeof(comm) - 1);
                close(fd);
                if (n <= 0)
                    continue;
                comm[n] = 0;
                if (char *nl = std::strchr(comm, '\n'))
                    *nl = 0;
                for (const char *want : kHintThreads)
                    if (std::strcmp(comm, want) == 0)
                    {
                        g_threads.push_back({tid, comm, threadCpuNs(tid)});
                        added = true;
                    }
            }
            closedir(d);
            return added;
        }

        void makeSession()
        {
            closeSession();
            const Api &a = api();
            HintManager *m = a.getManager ? a.getManager() : nullptr;
            if (!m || !a.createSession || !a.reportActual)
            {
                if (!g_failed)
                    std::fprintf(stderr, "[power] ADPF: no performance hint manager on this phone\n");
                g_failed = true;
                return;
            }
            std::vector<int32_t> tids;
            std::string names;
            for (const Tracked &t : g_threads)
            {
                tids.push_back(t.tid);
                names += (names.empty() ? "" : ", ") + t.name;
            }
            g_session = a.createSession(m, tids.data(), tids.size(), g_target);
            if (!g_session)
                std::fprintf(stderr, "[power] ADPF: the session for %s could not be made\n", names.c_str());
            else
                std::fprintf(stderr, "[power] ADPF session: %zu threads (%s), target %.2f ms\n", tids.size(), names.c_str(), g_target / 1e6);
        }

        void pollThermal(int64_t now)
        {
            const Api &a = api();
            if (!g_thermal && a.acquireThermal && !g_lastStatusPoll)
                g_thermal = a.acquireThermal();
            if (!g_thermal)
            {
                g_lastStatusPoll = now;
                return;
            }
            if (now - g_lastStatusPoll >= 1000000000ll && a.thermalStatus)
            {
                g_lastStatusPoll = now;
                const int s = a.thermalStatus(g_thermal);
                const int was = g_status.exchange(s, std::memory_order_relaxed);
                if (s != was)
                {
                    const float h = g_headroom.load(std::memory_order_relaxed);
                    std::fprintf(stderr, "[power] thermal status %s (headroom %.2f)\n", statusName(s), h);
                }
            }
            if (now - g_lastHeadroomPoll >= 5000000000ll && a.thermalHeadroom)
            {
                g_lastHeadroomPoll = now;
                const float h = a.thermalHeadroom(g_thermal, 10);
                if (!std::isnan(h))
                    g_headroom.store(h, std::memory_order_relaxed);
            }
        }
    }

    void powerHostFrame(uint64_t periodNs)
    {
        const int64_t now = monoNs();
        if (!g_started)
        {
            g_started = now;
            g_threads.push_back({gettid(), "main", threadCpuNs(gettid())});
            if (envOff("VCS_ADPF"))
                g_hintsOn.store(false, std::memory_order_relaxed);
        }
        pollThermal(now);

        // the log: what the session was told, and the phone's state (every 30 s)
        if (now - g_lastLog >= 30000000000ll)
        {
            if (g_lastLog && g_busyFrames)
                std::fprintf(stderr, "[power] last 30 s: busiest thread %.1f ms a frame (max %.1f) of %.1f, ADPF %s, thermal %s, headroom %.2f%s\n",
                             g_busySumMs / g_busyFrames, g_busyMaxMs, periodNs / 1e6, g_session ? "on" : "off",
                             statusName(g_status.load(std::memory_order_relaxed)), g_headroom.load(std::memory_order_relaxed),
                             g_cooling.load(std::memory_order_relaxed) ? ", cooling (30 fps)" : "");
            g_lastLog = now;
            g_busySumMs = g_busyMaxMs = 0.0;
            g_busyFrames = 0;
        }

        // the threads: looked for every 2 s for the first two minutes (the runtime starts them as the game boots)
        if (g_threads.size() < kWanted && now - g_started < 120000000000ll && now - g_lastScan >= 2000000000ll)
        {
            g_lastScan = now;
            if (scanThreads())
            {
                closeSession(); // made again below with the new set (setThreads is Android 14+ only)
                g_failed = false;
            }
        }

        // the busiest thread's CPU time since the last frame (also kept while the hints are off: the log has it)
        int64_t busiest = 0;
        for (Tracked &t : g_threads)
        {
            const int64_t cpu = threadCpuNs(t.tid);
            if (cpu < 0)
                continue;
            if (t.lastCpu >= 0)
                busiest = std::max(busiest, cpu - t.lastCpu);
            t.lastCpu = cpu;
        }
        const int64_t since = g_lastReport ? now - g_lastReport : 0;
        g_lastReport = now;
        if (since <= 0 || since > 1000000000ll)
            return; // the first frame, or after a long stall (a load, the background): nothing meaningful to report
        busiest = std::clamp<int64_t>(busiest, 1000, since);
        g_busySumMs += busiest / 1e6;
        g_busyMaxMs = std::max(g_busyMaxMs, busiest / 1e6);
        ++g_busyFrames;

        if (!g_hintsOn.load(std::memory_order_relaxed))
        {
            if (g_session)
            {
                closeSession();
                std::fprintf(stderr, "[power] ADPF session closed (performance hints off)\n");
            }
            return;
        }
        if (g_failed)
            return;
        const int64_t target = static_cast<int64_t>(std::max<uint64_t>(periodNs, 1000000u));
        if (!g_session)
        {
            g_target = target;
            makeSession();
            if (!g_session)
            {
                g_failed = true; // not again every frame
                return;
            }
        }
        else if (target != g_target && api().updateTarget)
        {
            g_target = target;
            api().updateTarget(g_session, target);
        }
        const bool efficiency = g_preferEfficiency.load(std::memory_order_relaxed);
        if (efficiency != g_efficiencyApplied && api().preferEfficiency)
        {
            api().preferEfficiency(g_session, efficiency);
            g_efficiencyApplied = efficiency;
        }
        api().reportActual(g_session, busiest);
    }

    void setPerformanceHints(bool on)
    {
        if (on && envOff("VCS_ADPF"))
            on = false;
        g_hintsOn.store(on, std::memory_order_relaxed);
    }

    void setPreferPowerEfficiency(bool on) { g_preferEfficiency.store(on, std::memory_order_relaxed); }

    int thermalStatus() { return g_status.load(std::memory_order_relaxed); }
    float thermalHeadroom() { return g_headroom.load(std::memory_order_relaxed); }

    void setCooling(bool on) { g_cooling.store(on, std::memory_order_relaxed); }
    bool cooling() { return g_cooling.load(std::memory_order_relaxed); }

    namespace
    {
        std::atomic<float> g_refreshHz{0.0f};
    }

    float displayRefreshHz() { return g_refreshHz.load(std::memory_order_relaxed); }

    void noteDisplayRefresh(float hz)
    {
        if (!(hz > 1.0f && hz < 1000.0f))
            return;
        if (std::fabs(g_refreshHz.exchange(hz, std::memory_order_relaxed) - hz) > 0.5f)
            std::fprintf(stderr, "[power] display refresh %.1f Hz\n", hz);
    }
}

extern "C"
{
    // VCSActivity: the display's refresh rate, at the start and whenever the display changes (a mode switch, a fold)
    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_VCSActivity_nativeDisplayRefresh(JNIEnv *, jclass, jfloat hz)
    {
        vcs_android::noteDisplayRefresh(hz);
    }
}
