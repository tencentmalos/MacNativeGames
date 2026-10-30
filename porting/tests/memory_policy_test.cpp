// SPDX-License-Identifier: MIT
#include "../nvn_vulkan/memory_policy.h"
#include <cassert>
int main(){
    VkPhysicalDeviceMemoryProperties m{};m.memoryTypeCount=4;
    m.memoryTypes[0].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    m.memoryTypes[1].propertyFlags=m.memoryTypes[0].propertyFlags|VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    m.memoryTypes[2].propertyFlags=m.memoryTypes[1].propertyFlags&~VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    m.memoryTypes[3].propertyFlags=VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT|VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    using mng::vulkan::upload_memory_type;
    assert(upload_memory_type(m,15,true)==1); // UMA coherent/cached wins.
    assert(upload_memory_type(m,15,false)==0); // Exact old allocation policy.
    assert(upload_memory_type(m,13,true)==0); // Resource excludes cached type.
    assert(upload_memory_type(m,4,true)==2); // No coherent memory: caller flushes.
    assert(upload_memory_type(m,8,true)==UINT32_MAX); // Never map private-only.
    m.memoryTypes[1].propertyFlags&=~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    assert(upload_memory_type(m,15,true)==0); // No cached system-heap preference.
}
