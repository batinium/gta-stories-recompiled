// [android] LSFG frame generation in VulkanDevice's present (docs/ANDROID.md "Frame generation (LSFG)"; framegen.h).
// Included inside the class in rhi_vulkan.cpp, so the backend's own code only calls these at a few marked places:
// fgDeviceSetup / fgDeviceMade (createDevice), fgSwapchain / fgSwapchainMade (createSwapchain), framegenPresent and
// framegenTimes (present). Without VCS_LSFG only the empty versions at the end are compiled.
//
// present() with frame generation: the window's frame (the presenter's last pass) is in swapchain image A. The frame
// generation copies A into its chain and records its passes into the frame's command buffer, which is submitted at once.
// Each generated frame g then gets an image of its own: acquired (B), the output blitted into it, presented with a
// display time (VK_GOOGLE_display_timing) one hold after the image before it. Image A, the game's frame, is presented
// last by present() itself, with its time from framegenTimes(). FIFO: each image stays a refresh at least, and the
// acquires make the game thread wait for the display (the pacing). Two images are held at once (A and B): the swapchain
// gets one more image than the surface's minimum.
#if defined(VCS_LSFG)
            std::unique_ptr<lsfg::FrameGen> m_fg;
            lsfg::Needs m_fgNeeds;
            bool m_fgWanted = false;      // a Lossless.dll and the device has the features (they are enabled)
            bool m_fgOnSwap = false;      // the swapchain was made for frame generation (switched live: fgCheckLive)
            bool m_displayTiming = false; // VK_GOOGLE_display_timing enabled
            uint64_t m_refreshNs = 0, m_lastDesired = 0;
            uint32_t m_presentId = 0, m_refreshPolls = 0;
            int m_frameRateSet = 0;
            VkPresentTimeGOOGLE m_fgTime{};
            VkPresentTimesInfoGOOGLE m_fgTimes{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};
            bool m_fgTimed = false; // the next present of the game's frame carries m_fgTime
            struct FgSemaphores // one per generated frame and frame slot: an acquire's semaphore is free again once its frame's fence passed
            {
                VkDevice device = VK_NULL_HANDLE;
                VkSemaphore s[kFrames][lsfg::kMaxGenerations]{};
                ~FgSemaphores()
                {
                    for (auto &slot : s)
                        for (VkSemaphore x : slot)
                            if (x)
                                vkDestroySemaphore(device, x, nullptr);
                }
            } m_fgSemaphores;

            static uint64_t fgNowNs()
            {
                timespec t{};
                clock_gettime(CLOCK_MONOTONIC, &t); // display timing's clock on Android
                return static_cast<uint64_t>(t.tv_sec) * 1000000000ull + static_cast<uint64_t>(t.tv_nsec);
            }

            // createDevice, before vkCreateDevice: the shaders' features and the extensions, whenever the GPU has them - with
            // or without a Lossless.dll, on or off: the side panel switches it on while playing, also with a DLL the
            // launcher got after the game started (fgCheckLive). The features change nothing for the game's own drawing.
            // VCS_LSFG_FEATURES=0: only with a DLL at the start (before 2026-11-18; a DLL added later then waits for a restart)
            void fgDeviceSetup(VkPhysicalDeviceFeatures2 &f2, VkPhysicalDeviceVulkan12Features &f12, std::vector<const char *> &extensions)
            {
                static const bool always = [] {
                    const char *e = std::getenv("VCS_LSFG_FEATURES");
                    return !(e && e[0] == '0');
                }();
                if ((!always && platform::frameGenDll().empty()) || m_offscreen)
                    return;
                if (!lsfg::requestFeatures(m_core->physical, f2, f12, m_fgNeeds))
                {
                    std::fprintf(stderr, "[lsfg] frame generation unavailable: the GPU lacks %s\n", m_fgNeeds.missing);
                    return;
                }
                if (m_fgNeeds.robustnessExt)
                    extensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
                if (deviceHas(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME))
                {
                    extensions.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
                    m_displayTiming = true;
                }
                m_fgWanted = true;
            }

            // createDevice, after volkLoadDevice: the DLL is read (when there is one already)
            void fgDeviceMade()
            {
                if (!m_fgWanted)
                    return;
                if (platform::frameGenDll().empty())
                {
                    std::fprintf(stderr, "[lsfg] frame generation ready for a Lossless.dll added while playing (the GPU's features are on)\n");
                    return;
                }
                fgCreate();
            }

            // The frame generation itself (the DLL read): at the device's creation, or while playing once the side panel
            // switches it on with a DLL the game did not have at its start (fgCheckLive). m_fgDll: the DLL tried last
            std::string m_fgDll;
            bool fgCreate()
            {
                m_fgDll = platform::frameGenDll();
                m_fg.reset(lsfg::FrameGen::create(m_core->physical, m_core->device, m_queue, m_queueFamily));
                if (!m_fg)
                    return false;
                if (!m_fgSemaphores.device)
                {
                    m_fgSemaphores.device = m_core->device;
                    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
                    for (auto &slot : m_fgSemaphores.s)
                        for (VkSemaphore &x : slot)
                            vkCreateSemaphore(m_core->device, &si, nullptr, &x);
                }
                std::fprintf(stderr, "[lsfg] frame generation on: FIFO present%s\n",
                             m_displayTiming ? ", paced with VK_GOOGLE_display_timing" : " (no VK_GOOGLE_display_timing: not paced)");
                return true;
            }

            // createSwapchain, before vkCreateSwapchainKHR: the images are copied from (into the chain), FIFO, one more image
            void fgSwapchain(const VkSurfaceCapabilitiesKHR &caps, VkSwapchainCreateInfoKHR &ci, VkPresentModeKHR &mode)
            {
                m_fgOnSwap = m_fg && lsfg::liveOn();
                if (!m_fgOnSwap)
                    return;
                if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
                {
                    std::fprintf(stderr, "[lsfg] frame generation unavailable: the window's images can't be copied from (no TRANSFER_SRC)\n");
                    m_fg.reset();
                    m_fgOnSwap = false;
                    return;
                }
                ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
                mode = ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
                uint32_t images = std::max(ci.minImageCount, caps.minImageCount + 1);
                if (caps.maxImageCount)
                    images = std::min(images, caps.maxImageCount);
                ci.minImageCount = images;
            }

            // createSwapchain, after: the display's refresh, a new history
            void fgSwapchainMade()
            {
                if (!m_fgOnSwap)
                    return;
                m_fg->reset();
                m_lastDesired = 0;
                m_fgTimed = false;
                m_refreshNs = 0;
                fgReadRefresh();
            }

            void fgReadRefresh()
            {
                VkRefreshCycleDurationGOOGLE rc{};
                if (!m_displayTiming || !vkGetRefreshCycleDurationGOOGLE || !m_swapchain ||
                    vkGetRefreshCycleDurationGOOGLE(m_core->device, m_swapchain, &rc) != VK_SUCCESS || !rc.refreshDuration)
                    return;
                const uint64_t a = rc.refreshDuration, b = m_refreshNs, d = a > b ? a - b : b - a;
                if (!b || d * 100 >= b) // the driver's estimate drifts by 0.01-0.02 Hz: only a real change is logged
                {
                    std::fprintf(stderr, "[lsfg] display refresh %.2f Hz\n", 1e9 / static_cast<double>(a));
                    m_lastDesired = 0;
                }
                m_refreshNs = a;
            }

            // The window's frame rate (ANativeWindow_setFrameRate through the platform layer): the output rate, so the system
            // may raise the panel's refresh for the generated frames (a 60 fps game x2 needs 120 Hz)
            void fgFrameRate()
            {
                // snapped up to a standard panel rate: the measured rate wanders (74, 81, 88...) and each new value made
                // the system pick another mode, so the panel often sat at 60 Hz and the generated frames had no slot
                int rate = m_fg->outputRate();
                if (rate)
                    for (int r : {30, 60, 90, 120, 144, 165, 240})
                        if (r >= rate - 2) { rate = r; break; }
                if (!rate || rate == m_frameRateSet || !m_setFrameRate)
                    return;
                m_frameRateSet = rate;
                m_setFrameRate(static_cast<float>(rate));
                m_refreshPolls = 0; // the panel may switch within a few frames: read the refresh soon
                std::fprintf(stderr, "[lsfg] window frame rate %d fps asked for\n", rate);
            }

            // The display time of the next image: `holdNs` after the one before (at least a refresh), half a refresh early
            // (a time in the middle of the refresh it should land on)
            uint64_t fgDesired(uint64_t holdNs)
            {
                if (!m_displayTiming || !m_refreshNs)
                    return 0;
                const uint64_t period = std::max(holdNs, m_refreshNs), now = fgNowNs();
                uint64_t want = m_lastDesired ? m_lastDesired + period : now;
                if (want < now)
                    want = now;
                m_lastDesired = want;
                return want - m_refreshNs / 2;
            }

            // Every image transition of this file: after everything before, before everything after (a few a frame)
            void fgImageBarrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to)
            {
                VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                b.oldLayout = from;
                b.newLayout = to;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = image;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                di.imageMemoryBarrierCount = 1;
                di.pImageMemoryBarriers = &b;
                vkCmdPipelineBarrier2(cb, &di);
                ++m_stats.barriers;
            }

            // One generated frame: an image acquired, the output blitted into it, presented. False: no image (stop here)
            bool fgPresentGenerated(int g, uint64_t holdNs)
            {
                VkSemaphore acquired = m_fgSemaphores.s[m_core->frame][g];
                VkImage output = m_fg->output(g);
                uint32_t index = kNoImage;
                const auto t0 = std::chrono::steady_clock::now();
                // a timeout, not forever: two held images need the swapchain's extra one (fgSwapchain)
                VkResult r = vkAcquireNextImageKHR(m_core->device, m_swapchain, 1000000000ull, acquired, VK_NULL_HANDLE, &index);
                m_stats.acquireMs += msSince(t0);
                if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
                {
                    if (r == VK_ERROR_OUT_OF_DATE_KHR)
                        m_swapchainDirty = true;
                    else
                    {
                        static int told = 0;
                        if (told++ < 4)
                            std::fprintf(stderr, "[lsfg] no swapchain image for a generated frame: %s\n", vkResultName(r));
                    }
                    return false;
                }
                VkCommandBuffer cb = newCommandBuffer();
                VkImage image = m_swapImages[index];
                fgImageBarrier(cb, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); // all of it is written
                VkImageBlit b{};
                b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                b.srcOffsets[1] = b.dstOffsets[1] = {static_cast<int32_t>(m_swapExtent.width), static_cast<int32_t>(m_swapExtent.height), 1};
                vkCmdBlitImage(cb, output, VK_IMAGE_LAYOUT_GENERAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
                fgImageBarrier(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                m_swapLayouts[index] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                vkEndCommandBuffer(cb);
                const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
                VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                si.waitSemaphoreCount = 1;
                si.pWaitSemaphores = &acquired;
                si.pWaitDstStageMask = &stage;
                si.commandBufferCount = 1;
                si.pCommandBuffers = &cb;
                si.signalSemaphoreCount = 1;
                si.pSignalSemaphores = &m_renderDone[index];
                r = vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE);
                if (r != VK_SUCCESS)
                {
                    reportLost(r, "vkQueueSubmit (generated frame)");
                    return false;
                }
                VkPresentTimeGOOGLE time{++m_presentId, fgDesired(holdNs)};
                VkPresentTimesInfoGOOGLE times{VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE};
                times.swapchainCount = 1;
                times.pTimes = &time;
                VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
                pi.pNext = m_displayTiming ? &times : nullptr;
                pi.waitSemaphoreCount = 1;
                pi.pWaitSemaphores = &m_renderDone[index];
                pi.swapchainCount = 1;
                pi.pSwapchains = &m_swapchain;
                pi.pImageIndices = &index;
                const auto t1 = std::chrono::steady_clock::now();
                r = vkQueuePresentKHR(m_queue, &pi);
                m_stats.presentMs += msSince(t1);
                if (r == VK_ERROR_OUT_OF_DATE_KHR || (r == VK_SUBOPTIMAL_KHR && !m_suboptimalExpected))
                    m_swapchainDirty = true;
                else if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR && r != VK_ERROR_SURFACE_LOST_KHR)
                    reportLost(r, "vkQueuePresentKHR (generated frame)");
                return r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
            }

            // present(), after the last pass: the chain's work for the window's frame and the generated frames before it.
            // The game's frame itself is presented by present() (with framegenTimes)
            void framegenPresent()
            {
                m_fgTimed = false;
                if (!m_fg || !m_fgOnSwap || m_imageIndex == kNoImage || !m_recording)
                    return;
                if (++m_refreshPolls % 120 == 30)
                    fgReadRefresh(); // the panel may change on its own (LTPO, thermal) or after the frame rate asked for
                outsideRendering(false);
                const uint32_t first = m_imageIndex;
                fgImageBarrier(m_cmd, m_swapImages[first], m_swapLayouts[first], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                uint64_t holdNs = 0;
                const int ng = m_fg->frame(m_cmd, m_swapImages[first], m_swapExtent, m_refreshNs, holdNs);
                fgFrameRate();
                fgImageBarrier(m_cmd, m_swapImages[first], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                m_swapLayouts[first] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                if (ng <= 0)
                {
                    m_lastDesired = 0; // untimed, as without frame generation
                    return;
                }
                // the frame and the chain's passes go first (they wait for image A); recording goes on in a new buffer
                submit(false, VK_NULL_HANDLE);
                beginCommands();
                for (int g = 0; g < ng; ++g)
                    if (!fgPresentGenerated(g, holdNs))
                        break;
                m_fgTime = {++m_presentId, fgDesired(holdNs)};
                m_fgTimes.swapchainCount = 1;
                m_fgTimes.pTimes = &m_fgTime;
                m_fgTimed = m_displayTiming;
            }

            // The pNext of the game's frame's VkPresentInfoKHR (null: no time)
            const void *framegenTimes() const { return m_fgTimed ? &m_fgTimes : nullptr; }

            // acquire, before the swapchain is looked at: switched on / off live (the side panel) -> a swapchain for it
            // (FIFO, copied from, one more image) or the plain one again; off hands the window's rate back to the platform
            void fgCheckLive()
            {
                // switched on without a frame generation yet: a Lossless.dll added since the start (the device has the
                // features: fgDeviceSetup asks for them whenever the GPU has them). Each DLL path is tried once
                if (!m_fg && m_fgWanted && m_swapchain && lsfg::liveOn())
                {
                    const std::string dll = platform::frameGenDll();
                    if (dll.empty() || dll == m_fgDll || !fgCreate())
                        return;
                    std::fprintf(stderr, "[lsfg] frame generation made while playing (%s)\n", dll.c_str());
                }
                if (!m_fg || !m_swapchain || m_swapchainDirty || lsfg::liveOn() == m_fgOnSwap)
                    return;
                m_swapchainDirty = true;
                m_lastDesired = 0;
                m_fgTimed = false;
                if (m_fgOnSwap)
                {
                    m_frameRateSet = 0;
                    if (m_setFrameRate)
                        m_setFrameRate(-1.0f);
                }
                std::fprintf(stderr, "[lsfg] frame generation switched %s: a new swapchain\n", m_fgOnSwap ? "off" : "on");
            }
#else
            void fgDeviceSetup(VkPhysicalDeviceFeatures2 &, VkPhysicalDeviceVulkan12Features &, std::vector<const char *> &)
            {
                const char *e = std::getenv("VCS_LSFG");
                if (e && *e && *e != '0')
                    std::fprintf(stderr, "[lsfg] frame generation unavailable: not in this build (built with VCS_LSFG off)\n");
            }
            void fgDeviceMade() {}
            void fgSwapchain(const VkSurfaceCapabilitiesKHR &, VkSwapchainCreateInfoKHR &, VkPresentModeKHR &) {}
            void fgSwapchainMade() {}
            void framegenPresent() {}
            const void *framegenTimes() const { return nullptr; }
            void fgCheckLive() {}
#endif
            void (*m_setFrameRate)(float fps) = nullptr; // [android] rhi::VulkanWindow::setFrameRate
