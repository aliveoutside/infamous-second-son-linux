// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_instance.h"

namespace iss {
/// The driver's current memory budget (VK_EXT_memory_budget): lower than the one read at start when
/// other programs hold VRAM. The heaps are the ones Instance counts: device local, all on an
/// integrated GPU. 0 without the extension.
inline u64 DeviceMemoryBudgetNow(const Vulkan::Instance& instance) {
    if (!instance.CanReportMemoryUsage()) return 0;
    vk::PhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    vk::PhysicalDeviceMemoryProperties2 props{.pNext = &budget};
    instance.GetPhysicalDevice().getMemoryProperties2(&props);
    u64 total = 0;
    for (u32 i = 0; i < props.memoryProperties.memoryHeapCount; ++i) {
        const bool local = bool(props.memoryProperties.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal);
        if (instance.IsIntegrated() || local) total += budget.heapBudget[i];
    }
    return total;
}
} // namespace iss
