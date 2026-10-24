/* SPDX-License-Identifier: MIT */
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Native-debugger reads this at an all-stop epoch, twice around the payload.
 * Both reads must match and sequence must be even. Never evaluate functions
 * in the stopped target to acquire a snapshot. */
typedef struct MngPortDebugDescriptor {
    uint32_t abi_version, descriptor_size;
    uint64_t sequence, generation, payload_address, payload_size;
} MngPortDebugDescriptor;
extern MngPortDebugDescriptor mng_port_debug_descriptor;
void mng_port_debug_checkpoint(void);
/* C loader adapter. Returns zero on failure; only the owning generation may
 * publish/close. No exceptions cross this API. Not signal-handler safe. */
uint64_t mng_port_debug_open(void);
int mng_port_debug_publish(uint64_t generation,const char* payload,size_t length);
void mng_port_debug_close(uint64_t generation);
#ifdef __cplusplus
}
#endif
