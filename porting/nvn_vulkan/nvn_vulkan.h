// SPDX-License-Identifier: MIT
#pragma once
#include "engine/xbg_game.h"
#ifdef __cplusplus
extern "C" {
#endif
// One serialized native game session. Vulkan objects are owned by the caller.
int mng_nvn_initialize(const xbg_game_config *config, char *error, size_t bytes);
void *mng_nvn_get_proc_address(const char *name);
size_t mng_nvn_proc_names(const char **names, size_t capacity);
int mng_nvn_shutdown(void);
// Bounded, synthetic GPU validation. Directory must be private and writable.
// Requires no active game/Vulkan session; never invokes guest code.
int mng_nvn_self_test(const char *directory, char *report, size_t report_bytes);
#ifdef __cplusplus
}
#endif
