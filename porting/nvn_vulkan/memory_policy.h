// SPDX-License-Identifier: MIT
#pragma once
#include <vulkan/vulkan.h>

namespace mng::vulkan {
// CPU-populated GPU buffers on UMA can be device-local, coherent AND cached.
// Prefer that combination when the resource supports it. Do not trade away
// coherence or use a non-device-local cached heap merely to gain CPU caching.
inline uint32_t upload_memory_type(const VkPhysicalDeviceMemoryProperties& memory,
                                   uint32_t bits, bool prefer_cached) {
    const VkMemoryPropertyFlags visible=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const VkMemoryPropertyFlags coherent=visible|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags unified=coherent|VK_MEMORY_PROPERTY_HOST_CACHED_BIT|
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const VkMemoryPropertyFlags preferences[]{prefer_cached?unified:coherent,coherent,visible};
    for(const auto flags:preferences)
        for(uint32_t i=0;i<memory.memoryTypeCount;++i)
            if((bits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags)return i;
    return UINT32_MAX;
}
}
