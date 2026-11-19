// SPDX-License-Identifier: GPL-3.0-or-later
// [android] See framegen.h. Ported from the earlier port's vka_framegen.c (GPL-3.0-or-later): the same
// chain, pacer, warm-up and refresh cap; without its Switch-specific parts (window texture formats, the host's frame limit,
// the in-game settings channel, the PNG dump). Here the input is the window's back buffer as the presenter drew it
// (the letterboxed picture and Dear ImGui), and the work goes into the frame's own command buffer.
#include "rhi/vulkan/framegen.h"

#include "platform/platform.h"

extern "C" {
#include "rhi/vulkan/lsfg/lsfg.h"
#include "rhi/vulkan/lsfg/lsfg_vk.h"
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <atomic>
#include <vector>

namespace rhi::lsfg
{
    namespace
    {
        int envInt(const char *name, int fallback)
        {
            const char *e = std::getenv(name);
            return e && *e ? std::atoi(e) : fallback;
        }

        double monoSeconds()
        {
            timespec t{};
            clock_gettime(CLOCK_MONOTONIC, &t);
            return t.tv_sec + t.tv_nsec / 1e9;
        }

        // An output image: storage (the generate pass writes it) and a blit source (into the swapchain image)
        bool makeImage(VkExtent2D e, VkImage &img, VkDeviceMemory &mem, VkImageView &view)
        {
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.format = VK_FORMAT_R8G8B8A8_UNORM;
            ci.extent = {e.width, e.height, 1};
            ci.mipLevels = ci.arrayLayers = 1;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            VkResult r = vkCreateImage(LV.dev, &ci, nullptr, &img);
            VkMemoryRequirements mr{};
            if (r == VK_SUCCESS)
                vkGetImageMemoryRequirements(LV.dev, img, &mr);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = lsfg_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            if (r == VK_SUCCESS && ai.memoryTypeIndex == UINT32_MAX)
                r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
            if (r == VK_SUCCESS)
                r = vkAllocateMemory(LV.dev, &ai, nullptr, &mem);
            if (r == VK_SUCCESS)
                r = vkBindImageMemory(LV.dev, img, mem, 0);
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = img;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = ci.format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (r == VK_SUCCESS)
                r = vkCreateImageView(LV.dev, &vi, nullptr, &view);
            if (r != VK_SUCCESS)
                std::fprintf(stderr, "[lsfg] error: output image %ux%u: %d\n", e.width, e.height, r);
            return r == VK_SUCCESS;
        }

        void memoryBarrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst, VkAccessFlags dstAccess)
        {
            VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            b.srcAccessMask = srcAccess;
            b.dstAccessMask = dstAccess;
            vkCmdPipelineBarrier(cb, src, dst, 0, 1, &b, 0, nullptr, 0, nullptr);
        }
    }

    const Settings &settings()
    {
        static const Settings s = [] {
            Settings v;
            v.on = envInt("VCS_LSFG", 0) != 0;
            v.multiplier = std::clamp(envInt("VCS_LSFG_MULT", 2), 2, 4);
            v.flow = std::clamp(envInt("VCS_LSFG_FLOW", 50), 25, 100);
            if (const char *d = std::getenv("VCS_LSFG_DLL"))
                v.dll = d;
            return v;
        }();
        return s;
    }

    bool liveOn() { return platform::frameGen(); }
    int liveMultiplier()
    {
        int m = 2;
        platform::frameGen(&m);
        return m;
    }
    int liveFlow()
    {
        int f = 50;
        platform::frameGen(nullptr, &f);
        return f;
    }

    bool requestFeatures(VkPhysicalDevice physical, VkPhysicalDeviceFeatures2 &f2, VkPhysicalDeviceVulkan12Features &f12, Needs &needs)
    {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> exts(n);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, exts.data());
        const bool hasRobustness2 = std::any_of(exts.begin(), exts.end(), [](const VkExtensionProperties &e) {
            return std::strcmp(e.extensionName, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) == 0;
        });
        VkPhysicalDeviceRobustness2FeaturesEXT rob{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
        VkPhysicalDeviceVulkan11Features h11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features h12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        h12.pNext = &h11;
        if (hasRobustness2)
            h11.pNext = &rob;
        VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        have.pNext = &h12;
        vkGetPhysicalDeviceFeatures2(physical, &have);
        needs.missing = !h12.shaderFloat16                                      ? "shaderFloat16"
                        : !have.features.shaderStorageImageExtendedFormats      ? "shaderStorageImageExtendedFormats"
                        : !have.features.shaderStorageImageWriteWithoutFormat   ? "shaderStorageImageWriteWithoutFormat"
                        : !hasRobustness2 || !rob.nullDescriptor                ? "null descriptors (VK_EXT_robustness2)"
                                                                                : nullptr;
        if (needs.missing)
            return false;
        // what the shaders need, and the 16-bit / 8-bit types and format-less reads where the device has them (the
        // performance set's FP16 modules may declare them); nothing else changes for the game's own drawing
        f12.shaderFloat16 = VK_TRUE;
        f12.shaderInt8 = h12.shaderInt8;
        f2.features.shaderStorageImageExtendedFormats = VK_TRUE;
        f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        f2.features.shaderStorageImageReadWithoutFormat = have.features.shaderStorageImageReadWithoutFormat;
        f2.features.shaderInt16 = have.features.shaderInt16;
        needs.f11.storageBuffer16BitAccess = h11.storageBuffer16BitAccess;
        needs.f11.uniformAndStorageBuffer16BitAccess = h11.uniformAndStorageBuffer16BitAccess;
        needs.f11.storagePushConstant16 = h11.storagePushConstant16;
        needs.f11.storageInputOutput16 = h11.storageInputOutput16;
        needs.robustness2.nullDescriptor = VK_TRUE; // only it: robust buffer / image access cost and nothing needs them
        needs.robustnessExt = true;
        needs.f11.pNext = &needs.robustness2;
        needs.robustness2.pNext = f12.pNext;
        f12.pNext = &needs.f11;
        return true;
    }

    struct lsfgState
    {
        lsfg_modules mods{};
        lsfg_chain *chain = nullptr;
        VkExtent2D ext{};
        VkImage out[LSFG_MAX_GENERATIONS]{};
        VkDeviceMemory outMem[LSFG_MAX_GENERATIONS]{};
        VkImageView outView[LSFG_MAX_GENERATIONS]{};
        lsfg_pacer pacer{};
        uint64_t count = 0;
        uint32_t warmStreak = 0;
        int state = 0; // what was last logged: 1 generating, 2 not generating (waiting / capped)
        int capped = 0;
        double longerSince = 0.0;
        int flow = 0; // the chain's (rebuild): a new one when the live flow changes
    };

    FrameGen *FrameGen::create(VkPhysicalDevice physical, VkDevice device, VkQueue queue, uint32_t family)
    {
        // the DLL as it is now: VCS_LSFG_DLL at the start, or the copy the side panel named since (a DLL added in the
        // launcher while the game ran: fgCheckLive creates the frame generation then)
        Settings s = settings();
        s.dll = platform::frameGenDll();
        LV.pd = physical;
        LV.dev = device;
        LV.q = queue;
        LV.qf = family;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical, &props);
        LV.ub_align = props.limits.minUniformBufferOffsetAlignment;
        for (VkFormat f : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT})
        {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(physical, f, &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
                !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
            {
                std::fprintf(stderr, "[lsfg] frame generation unavailable: the GPU lacks storage images of R8, RGBA8 or RGBA16F\n");
                return nullptr;
            }
        }
        if (s.dll.empty())
        {
            std::fprintf(stderr, "[lsfg] frame generation unavailable: no Lossless.dll (pick it in the launcher's Frame generation card; "
                                 "VCS_LSFG_DLL names it)\n");
            return nullptr;
        }
        const auto t0 = std::chrono::steady_clock::now();
        auto *st = new lsfgState();
        const int r = lsfg_dll_load(s.dll.c_str(), &st->mods);
        if (r != LSFG_OK)
        {
            std::fprintf(stderr, "[lsfg] frame generation unavailable: %s: %s\n", s.dll.c_str(), lsfg_status_name(r));
            delete st;
            return nullptr;
        }
        std::fprintf(stderr, "[lsfg] frame generation: %d shaders from %s (%.1f ms); x%d, flow %d%%\n", st->mods.n, s.dll.c_str(),
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), liveMultiplier(), liveFlow());
        auto *fg = new FrameGen();
        fg->m = st;
        return fg;
    }

    FrameGen::~FrameGen()
    {
        if (!m)
            return;
        teardown();
        lsfg_modules_free(&m->mods);
        delete m;
    }

    void FrameGen::teardown()
    {
        if (m->chain)
            lsfg_chain_free(m->chain);
        m->chain = nullptr;
        for (int i = 0; i < LSFG_MAX_GENERATIONS; ++i)
        {
            if (m->outView[i])
                vkDestroyImageView(LV.dev, m->outView[i], nullptr);
            if (m->out[i])
                vkDestroyImage(LV.dev, m->out[i], nullptr);
            if (m->outMem[i])
                vkFreeMemory(LV.dev, m->outMem[i], nullptr);
            m->outView[i] = VK_NULL_HANDLE;
            m->out[i] = VK_NULL_HANDLE;
            m->outMem[i] = VK_NULL_HANDLE;
        }
        m_input = VK_NULL_HANDLE;
        reset();
    }

    void FrameGen::reset()
    {
        lsfg_pacer_reset(&m->pacer);
        m->count = 0;
        m->warmStreak = 0;
    }

    bool FrameGen::rebuild(VkExtent2D e)
    {
        vkDeviceWaitIdle(LV.dev); // the chain being replaced may still be read
        teardown();
        for (int i = 0; i < LSFG_MAX_GENERATIONS; ++i)
            if (!makeImage(e, m->out[i], m->outMem[i], m->outView[i]))
            {
                teardown();
                return false;
            }
        const auto t0 = std::chrono::steady_clock::now();
        m->flow = liveFlow();
        m->chain = lsfg_chain_new(&m->mods, e, m->flow / 100.0f, m->out, m->outView);
        if (!m->chain)
        {
            teardown();
            return false;
        }
        m->ext = e;
        std::fprintf(stderr, "[lsfg] frame generation: chain for %ux%u made in %.1f ms\n", e.width, e.height,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        return true;
    }

    VkImage FrameGen::output(int g) const { return g >= 0 && g < LSFG_MAX_GENERATIONS ? m->out[g] : VK_NULL_HANDLE; }

    int FrameGen::frame(VkCommandBuffer cb, VkImage src, VkExtent2D e, uint64_t refreshNs, uint64_t &holdNs)
    {
        holdNs = 0;
        if (!m || m->state < 0)
            return 0;
        if (!m->chain || m->ext.width != e.width || m->ext.height != e.height || m->flow != liveFlow())
        {
            if (!rebuild(e))
            {
                std::fprintf(stderr, "[lsfg] frame generation unavailable: the chain could not be made (see the error lines above)\n");
                m->state = -1;
                return 0;
            }
        }
        const int mult = liveMultiplier();
        const double now = monoSeconds();
        const lsfg_plan plan = lsfg_pacer_plan(&m->pacer, LSFG_MAX_GENERATIONS, mult, 0, now);
        const uint64_t count = m->count++;
        const bool warm = plan.warm && count + 1 >= 2;
        m->warmStreak = warm ? m->warmStreak + 1 : 0;
        size_t ng = warm && m->warmStreak >= 2 ? plan.generations : 0;
        ng = std::min<size_t>(ng, LSFG_MAX_GENERATIONS);
        const double interval = m->pacer.smoothed_interval;

        // The rate to ask the window for: the game's own times the multiplier; up at once, down only after it held 2 s
        if (interval > 0.0)
        {
            const int want = std::clamp(static_cast<int>(mult / interval + 0.5), 30, 240);
            if (!m_outputRate || want > m_outputRate + 2)
            {
                m_outputRate = want;
                m->longerSince = 0.0;
            }
            else if (want < m_outputRate - 2)
            {
                if (!m->longerSince)
                    m->longerSince = now;
                else if (now - m->longerSince >= 2.0)
                {
                    m_outputRate = want;
                    m->longerSince = 0.0;
                }
            }
            else
                m->longerSince = 0.0;
        }

        // FIFO shows one image a refresh: no more images per game frame than refreshes in its interval (a 60 fps game
        // on a 60 Hz panel has no room for a generated frame; on 120 Hz one)
        int capped = 0;
        if (ng && refreshNs && interval > 0.0)
        {
            const double refreshes = interval * 1e9 / static_cast<double>(refreshNs);
            const size_t fit = refreshes < 1.9 ? 0 : static_cast<size_t>(refreshes + 0.1) - 1;
            if (ng > fit)
            {
                ng = fit;
                capped = 1;
                m->pacer.issued = ng; // the pacer counts what was generated
            }
        }

        // the window's frame (the presenter's last pass wrote it; earlier presents may still read the outputs)
        memoryBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        m_input = lsfg_chain_input(m->chain, count);
        VkImageBlit b{};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[1] = {static_cast<int32_t>(e.width), static_cast<int32_t>(e.height), 1};
        b.dstSubresource = b.srcSubresource;
        b.dstOffsets[1] = b.srcOffsets[1];
        // a blit, not a copy: the swapchain may be BGRA, the chain's input is RGBA
        vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_input, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
        if (warm)
            lsfg_chain_shared(m->chain, cb, count);
        for (size_t g = 0; g < ng; ++g)
            lsfg_chain_generate(m->chain, cb, count, ng, g);
        // the outputs and the input copy for the blits into the swapchain images after this
        memoryBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);

        if (ng)
        {
            // each image held for an equal share of the game's interval; within 0.15 refresh of a whole number of
            // refreshes it is that number (a hold a little over one refresh drifts across refreshes: two images in one)
            double hold = interval / static_cast<double>(ng + 1);
            if (refreshNs)
            {
                const double rr = hold * 1e9 / static_cast<double>(refreshNs), w = std::floor(rr + 0.5);
                if (w >= 1.0 && std::fabs(rr - w) <= 0.15)
                    hold = w * static_cast<double>(refreshNs) / 1e9;
            }
            holdNs = static_cast<uint64_t>(hold * 1e9);
        }

        const int state = ng ? 1 : 2;
        if (state != m->state || (state == 2 && capped != m->capped))
        {
            if (state == 1)
                std::fprintf(stderr, "[lsfg] frame generation active: %zu generated frame(s) per game frame, game frame %.1f ms, display %.1f Hz\n", ng,
                             interval * 1e3, refreshNs ? 1e9 / refreshNs : 0.0);
            else if (capped)
                std::fprintf(stderr, "[lsfg] frame generation on, not generating: the display runs at %.1f Hz, no refresh left for a generated frame "
                                     "(game frame %.1f ms; asking the window for %d fps)\n",
                             1e9 / refreshNs, interval * 1e3, m_outputRate);
            else
                std::fprintf(stderr, "[lsfg] frame generation on, waiting for a steady frame rate\n");
        }
        m->state = state;
        m->capped = capped;
        return static_cast<int>(ng);
    }
}
