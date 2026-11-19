// SPDX-License-Identifier: GPL-3.0-or-later
// [android] Lossless Scaling frame generation (LSFG) in the Vulkan backend's present (docs/ANDROID.md "Frame generation
// (LSFG)"): the port of the earlier port's vka_framegen.c over the chain in lsfg/ (GPL-3.0-or-later, lsfg/LICENSE.md).
// Built with VCS_LSFG only (CMake option, default ON on Android). The Vulkan backend (rhi_vulkan.cpp, framegen_present.inl)
// hands each finished window frame here before it is presented; the generated frames between the previous frame and this
// one are written to images of their own, which it presents before the frame itself. Off unless VCS_LSFG=1.
//
// Settings, read at the device's creation (VCSActivity sets them from the launcher's "Frame generation" card); on, the
// multiplier and the flow then change live (setLive: the side panel's Frame generation section):
//   VCS_LSFG=1           on
//   VCS_LSFG_MULT=2..4   images shown per game frame (2: one generated frame between two of the game's)
//   VCS_LSFG_FLOW=25..100  the optical flow's resolution in % of the frame (default 50; lower is cheaper, less exact)
//   VCS_LSFG_DLL=<path>  the user's own Lossless.dll (the launcher keeps a copy in files/lsfg/Lossless.dll); also named
//                        live (platform::setFrameGenDll), so a DLL added after the start is read when switched on: the
//                        device asks for the features whenever the GPU has them (VCS_LSFG_FEATURES=0: only with a DLL)
#pragma once

#include <volk.h>

#include <cstdint>
#include <string>

namespace rhi::lsfg
{
    constexpr int kMaxGenerations = 3; // generated frames per game frame at most (lsfg.h LSFG_MAX_GENERATIONS)

    struct Settings
    {
        bool on = false;
        int multiplier = 2; // 2..4
        int flow = 50;      // % (25..100)
        std::string dll;
    };
    const Settings &settings(); // from the environment, once
    // [android] While playing: the side panel's switch, multiplier and flow (platform::frameGen)
    bool liveOn();
    int liveMultiplier();
    int liveFlow();

    // What the shaders need of the device. `missing` names the first absent feature (null: all there)
    struct Needs
    {
        VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceRobustness2FeaturesEXT robustness2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
        const char *missing = nullptr;
        bool robustnessExt = false; // VK_EXT_robustness2 to enable
    };
    // Checks the device and, when it has everything, turns the features on in the device's create info (f2 / f12 are the
    // backend's own; f11 and robustness2 are chained after f12). False: `needs.missing` says why
    bool requestFeatures(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 &f2, VkPhysicalDeviceVulkan12Features &f12, Needs &needs);

    struct lsfgState;

    class FrameGen
    {
    public:
        // After the device was made with requestFeatures' features; reads the DLL. Null with the reason logged when
        // frame generation can't run (no DLL, not a usable one, formats missing)
        static FrameGen *create(VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family);
        ~FrameGen(); // the GPU must be idle

        // One game frame drawn into `src` (TRANSFER_SRC_OPTIMAL, `extent`): records into `cb` the copy into the chain's
        // input and its passes. Returns how many generated frames to show before it (output(0..n-1), in GENERAL layout
        // once `cb` ran) and `holdNs`, how long each image (the game's own frame too) should stay on screen.
        // refreshNs: the display's refresh (0: not known)
        int frame(VkCommandBuffer cb, VkImage src, VkExtent2D extent, uint64_t refreshNs, uint64_t &holdNs);
        VkImage output(int g) const;
        VkImage input() const { return m_input; } // the copy of the last frame (GENERAL)
        // The frame rate the window should ask the display for (0: none yet): the game's rate times the multiplier
        int outputRate() const { return m_outputRate; }
        void reset(); // a new swapchain / surface: the next frames start the history again

    private:
        FrameGen() = default;
        bool rebuild(VkExtent2D extent);
        void teardown();
        lsfgState *m = nullptr;
        VkImage m_input = VK_NULL_HANDLE;
        int m_outputRate = 0;
    };
}
