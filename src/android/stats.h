// [android] The stats overlay's native numbers (docs/ANDROID.md "Stats overlay"): the frame rate the host presents, the
// game's own frame rate, the frame time and the GPU's time a frame, counted here and read by the Java view
// (io.github.batinium.vcsrecomp.StatsOverlay) through JNI twice a second. Everything else it shows (CPU clock caps,
// thermals, battery, memory) is read in Java. A few atomics a frame; the GPU time only while the overlay asks for it.
#pragma once

#include <cstdint>

namespace vcs_android
{
    // Main thread, once per host frame (platform::beginFrame): a presented frame.
    void noteHostFrame();
    // Any thread, each host frame: the game's frame counter (vcs::rig::gameFrames), for its own frame rate. Never called
    // by the smoke test (no game: the overlay shows the presented rate alone).
    void noteGameFrames(uint64_t total);
    // The Vulkan RHI, once per finished frame while gpuTimingWanted(): the GPU's time from the frame's first command to
    // its last (timestamps), in ms.
    void noteGpuFrameMs(double ms);
    bool gpuTimingWanted();
    // [diag] The heartbeat / watchdog (diagnostics.cpp): every host frame since the start, and the longest gap between
    // two frames since the last call (ns; resets it). Atomics, any thread.
    uint64_t hostFrameTotal();
    int64_t takeMaxFrameGapNs();
}
