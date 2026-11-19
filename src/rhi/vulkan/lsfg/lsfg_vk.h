// SPDX-License-Identifier: GPL-3.0-or-later
// [android] The device the LSFG chain (lsfg_chain.c) works on: what the earlier port's vka.h gave it (the replay's device,
// queue, memory types, barriers), here filled by the Vulkan backend's frame generation (src/rhi/vulkan/framegen.cpp)
// before the chain is made. Vulkan entry points are volk's globals (VK_NO_PROTOTYPES, src/rhi/CMakeLists.txt). The chain
// is made and run on the RHI's one thread: no locks.
#pragma once
#include <volk.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue q;
    uint32_t qf;
    VkDeviceSize ub_align; // minUniformBufferOffsetAlignment
} lsfg_vk;
extern lsfg_vk LV;

// A memory type of `bits` with every flag of `want`; UINT32_MAX when there is none
uint32_t lsfg_mem_type(uint32_t bits, VkMemoryPropertyFlags want);
// A whole colour image from one layout to another, after and before all commands
void lsfg_img_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to);

#ifdef __cplusplus
}
#endif

#define LSFG_LOG(...) (fprintf(stderr, "[lsfg] " __VA_ARGS__), fputc('\n', stderr))
#define LSFG_ERR(...) (fprintf(stderr, "[lsfg] error: " __VA_ARGS__), fputc('\n', stderr))
