// SPDX-License-Identifier: MIT
#pragma once
#include <vulkan/vulkan.h>
// The driver and its namespace live until the dedicated game process exits.
const char* mng_vulkan_initialize(const char* kind, const char* hooks, const char* directory);
void mng_vulkan_identify(VkPhysicalDevice gpu);
