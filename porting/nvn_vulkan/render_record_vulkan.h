// SPDX-License-Identifier: MIT
#pragma once
#include <vulkan/vulkan.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Explicit opt-in. All calls belong to the serialized rendering owner.
int mng_render_record_initialize(VkInstance,VkPhysicalDevice,VkDevice,VkQueue,const char* private_directory);
int mng_render_record_request(uint64_t request,uint32_t frames,uint64_t max_bytes);
void mng_render_record_present(uint64_t completed_frame);
void mng_render_record_shutdown(void);
int mng_render_record_active(void);
void mng_render_record_action(uint64_t submission,uint64_t command,uint32_t opcode,uint32_t bytes,const char* name,const char* detail);
void mng_render_record_gpu_label(VkCommandBuffer command);
#ifdef __cplusplus
}
#endif
