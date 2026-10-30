// SPDX-License-Identifier: MIT
#pragma once
#include <vulkan/vulkan.h>
#include <cstddef>
void* mng_host_window();
const char* mng_host_surface_extension();
VkResult mng_host_create_surface(VkInstance,VkSurfaceKHR*);
VkExtent2D mng_host_surface_extent();
void mng_host_option(const char*,char*,size_t);
const char* mng_host_font();
void mng_host_capture_input(bool);
void mng_host_presented();
