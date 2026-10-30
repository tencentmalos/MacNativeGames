// SPDX-License-Identifier: MIT
#pragma once
#include "overlay.h"
#include <vulkan/vulkan.h>
#include <span>
namespace mng::overlay {
void initializeVulkan(const Config&,VkInstance,VkPhysicalDevice,VkDevice,VkQueue,uint32_t,
                      VkFormat,VkExtent2D,std::span<const VkImage>);
// Caller transitions the target to COLOR_ATTACHMENT_OPTIMAL, then to PRESENT.
void drawVulkan(VkCommandBuffer,uint32_t image);
}
