// SPDX-License-Identifier: MIT
#pragma once
#include "engine/xbg_game.h"
#ifdef __cplusplus
extern "C" {
#endif
// One serialized native game session. Vulkan objects are owned by the caller.
int mng_nvn_initialize(const xbg_game_config *config, char *error, size_t bytes);
void mng_nvn_set_host_wrapper(void *(*wrapper)(void *, const char *));
void mng_nvn_set_memory_validator(int (*validator)(uint64_t, size_t));
// Call before any guest rendering, only after enabling the actual device feature.
void mng_nvn_set_custom_border_color(int enabled);
// Configure before rendering: present submits under the engine lock; this
// callback waits only for its own GPU copy, without accessing renderer state.
void mng_nvn_set_present_wait(void (*callback)(void));
// Host present commands share the engine queue. Prepare the pinned source in
// the host command buffer and submit it before returning from cfg.present.
void mng_nvn_set_present_source_commands(void);
void mng_nvn_prepare_present_source(VkCommandBuffer command,VkImage source);
void *mng_nvn_get_proc_address(const char *name);
size_t mng_nvn_proc_names(const char **names, size_t capacity);
int mng_nvn_shutdown(void);
int mng_nvn_invalidate_control(int enabled,char *out,unsigned bytes);
int mng_nvn_publication_control(int enabled,char *out,unsigned bytes);
typedef struct {
    uint64_t host_objects,texture_snapshots,replay_texture_refs,command_handles,command_bytes;
    uint64_t debug_labels,texture_initializations,texture_finalizations;
    uint64_t image_identities,physical_images;
    uint64_t object_counts[8];
} mng_nvn_resources;
// Nonblocking: -1 means retry later; no GPU drain or guest session lock wait.
int mng_nvn_resource_stats(mng_nvn_resources *out);
int mng_nvn_resource_snapshot(char *out,unsigned bytes);
// Bounded, synthetic GPU validation. Directory must be private and writable.
// Requires no active game/Vulkan session; never invokes guest code.
int mng_nvn_self_test(const char *directory, char *report, size_t report_bytes);
#ifdef __cplusplus
}
#endif
