// [android] See stats.h. The host frames are counted on the main thread and published every half second as atomics; the
// GPU's frame times are summed by the render thread and taken at each publish; the Java view reads the last published
// numbers (nativeFrameStats). Nothing here allocates or locks.
#include "android/stats.h"
#include "android/power.h"

#include <jni.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>

namespace vcs_android
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        int64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }

        // main thread only (noteHostFrame)
        int64_t g_windowStart = 0, g_lastFrame = 0, g_maxGap = 0;
        uint32_t g_frames = 0;
        uint64_t g_gameAtStart = 0;

        // the game's counter, as last noted (any thread)
        std::atomic<uint64_t> g_gameFrames{0};
        std::atomic<bool> g_gameSeen{false};

        // the GPU's frame times since the last publish: microseconds summed, frames
        std::atomic<bool> g_overlayOn{false}, g_gpuWanted{false};
        std::atomic<uint64_t> g_gpuUs{0};
        std::atomic<uint32_t> g_gpuN{0};

        // published (the view reads these): fps x 100, ms x 1000, -1 for none
        std::atomic<int32_t> g_pubFps{0}, g_pubGameFps{-1}, g_pubFrameUs{0}, g_pubMaxUs{0}, g_pubGpuUs{-1};
        std::atomic<int64_t> g_pubAt{0};
        int64_t g_logAt = 0;

        constexpr int64_t kWindowNs = 500000000;   // a publish every half second
        constexpr int64_t kStaleNs = 1500000000;   // no host frame for this long (a load, the background): nothing to show
    }

    void noteHostFrame()
    {
        const int64_t t = nowNs();
        if (g_lastFrame && t - g_lastFrame < kStaleNs)
        {
            g_maxGap = std::max(g_maxGap, t - g_lastFrame);
            ++g_frames;
        }
        else // the first frame, or the first after a long gap: a new window
        {
            g_windowStart = t;
            g_frames = 0;
            g_maxGap = 0;
            g_gameAtStart = g_gameFrames.load(std::memory_order_relaxed);
        }
        g_lastFrame = t;
        const int64_t span = t - g_windowStart;
        if (span < kWindowNs)
            return;
        const double s = span / 1e9;
        g_pubFps.store(static_cast<int32_t>(g_frames / s * 100.0 + 0.5), std::memory_order_relaxed);
        g_pubFrameUs.store(g_frames ? static_cast<int32_t>(span / 1000 / g_frames) : 0, std::memory_order_relaxed);
        g_pubMaxUs.store(static_cast<int32_t>(g_maxGap / 1000), std::memory_order_relaxed);
        const uint64_t game = g_gameFrames.load(std::memory_order_relaxed);
        g_pubGameFps.store(g_gameSeen.load(std::memory_order_relaxed) ? static_cast<int32_t>((game - g_gameAtStart) / s * 100.0 + 0.5) : -1,
                           std::memory_order_relaxed);
        const uint32_t n = g_gpuN.exchange(0, std::memory_order_relaxed);
        const uint64_t us = g_gpuUs.exchange(0, std::memory_order_relaxed);
        g_pubGpuUs.store(n && g_gpuWanted.load(std::memory_order_relaxed) ? static_cast<int32_t>(us / n) : -1, std::memory_order_relaxed);
        g_pubAt.store(t, std::memory_order_relaxed);
        // the log gets the same numbers now and then while the overlay is on (a slow stretch in a log, matched later)
        if (g_overlayOn.load(std::memory_order_relaxed) && t - g_logAt >= 15 * 1000000000ll)
        {
            g_logAt = t;
            std::fprintf(stderr, "[stats] %.1f fps presented, game %.1f, frame %.1f ms (max %.1f), GPU %.1f ms\n", g_frames / s,
                         g_gameSeen.load(std::memory_order_relaxed) ? (game - g_gameAtStart) / s : -1.0, g_frames ? span / 1e6 / g_frames : 0.0,
                         g_maxGap / 1e6, n ? us / 1000.0 / n : -1.0);
        }
        g_windowStart = t;
        g_frames = 0;
        g_maxGap = 0;
        g_gameAtStart = game;
    }

    void noteGameFrames(uint64_t total)
    {
        g_gameFrames.store(total, std::memory_order_relaxed);
        g_gameSeen.store(true, std::memory_order_relaxed);
    }

    void noteGpuFrameMs(double ms)
    {
        if (!(ms >= 0.0 && ms < 1000.0))
            return;
        g_gpuUs.fetch_add(static_cast<uint64_t>(ms * 1000.0), std::memory_order_relaxed);
        g_gpuN.fetch_add(1, std::memory_order_relaxed);
    }

    bool gpuTimingWanted() { return g_gpuWanted.load(std::memory_order_relaxed); }
}

using namespace vcs_android;

extern "C"
{
    // out: presented fps, the game's fps (-1: no game), frame time ms (average), the longest frame ms, GPU ms a frame (-1:
    // none), cooling (1: the frame rate held at 30 while the phone is hot, android/cooling.cpp); false when nothing was
    // published lately (nothing presented: a load, the background)
    JNIEXPORT jboolean JNICALL Java_io_github_batinium_vcsrecomp_StatsOverlay_nativeFrameStats(JNIEnv *env, jclass, jfloatArray out)
    {
        const int64_t at = g_pubAt.load(std::memory_order_relaxed);
        const bool fresh = at && nowNs() - at < kStaleNs + kWindowNs;
        const int32_t game = g_pubGameFps.load(std::memory_order_relaxed), gpu = g_pubGpuUs.load(std::memory_order_relaxed);
        const jfloat v[6] = {
            g_pubFps.load(std::memory_order_relaxed) / 100.0f,
            game < 0 ? -1.0f : game / 100.0f,
            g_pubFrameUs.load(std::memory_order_relaxed) / 1000.0f,
            g_pubMaxUs.load(std::memory_order_relaxed) / 1000.0f,
            gpu < 0 ? -1.0f : gpu / 1000.0f,
            cooling() ? 1.0f : 0.0f,
        };
        if (out)
            env->SetFloatArrayRegion(out, 0, std::min<jsize>(6, env->GetArrayLength(out)), v);
        return fresh ? JNI_TRUE : JNI_FALSE;
    }

    // the overlay is on (the log gets its numbers every 15 s) / shows the GPU's time (the RHI writes a timestamp at each
    // frame's start and reads them back)
    JNIEXPORT void JNICALL Java_io_github_batinium_vcsrecomp_StatsOverlay_nativeSetWanted(JNIEnv *, jclass, jboolean overlay, jboolean gpu)
    {
        g_overlayOn.store(overlay != JNI_FALSE, std::memory_order_relaxed);
        if (g_gpuWanted.exchange(gpu != JNI_FALSE, std::memory_order_relaxed) != (gpu != JNI_FALSE))
            std::fprintf(stderr, "[stats] GPU frame timing %s\n", gpu ? "on" : "off");
    }
}
