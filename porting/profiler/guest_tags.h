// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
#include <string>
namespace mng::profile { std::string guestTagsCommand(const std::string& action); }
extern "C" {
#endif
// Startup-only installation in private RW memory, before any Guest owner runs.
// The caller validates the complete source/rewrite and seals the extra page RX.
int mng_guest_tags_requested(void);
int mng_guest_tags_install(uint8_t* image,size_t file_size,size_t code_offset,
    size_t allocation_size,const char* build_id,const char* source_sha256,
    void*(*wrap)(void*,const char*),uint32_t(*owner)(void));
const char* mng_guest_tags_error(void);
void mng_guest_tags_close(void); // Only after every Guest owner has stopped.
uint64_t mng_guest_tag_begin(unsigned id,uintptr_t caller,uintptr_t sp,const uint64_t* args);
void mng_guest_tag_end(uint64_t invocation);
#ifdef __cplusplus
}
#endif
