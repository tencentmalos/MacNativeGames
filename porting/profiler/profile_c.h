// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Never call from a signal handler. Tokens belong to the physical calling thread.
uint64_t mng_profile_begin(const char* name);
void mng_profile_end(uint64_t token);
typedef struct { uint64_t id, generation; } mng_profile_flow;
// A Post is paired only within the same recording generation.
mng_profile_flow mng_profile_post(const char* name);
uint64_t mng_profile_flow_begin(const char* name, mng_profile_flow flow);
void mng_profile_counter(const char* name, int64_t value);
extern __attribute__((visibility("hidden"))) unsigned mng_profile_hle_active;
// Zero when disabled; a new epoch invalidates outstanding diagnostic pairs.
uint64_t mng_profile_guest_epoch(void);
uint64_t mng_profile_address_epoch(void);
void mng_profile_address_event(const char* name,uint64_t epoch,uint64_t token,
    uint64_t wait_id,uint64_t address,uint32_t waiter,uint32_t peer,int64_t result);
static inline int mng_profile_guest_svc(unsigned svc) { return svc==0x1c || svc==0x1d; }
void mng_profile_guest_event(const char* name,uint64_t epoch,uint64_t token,
                             uint64_t wait_id,uint32_t waiter,uint32_t peer,int64_t result);
#ifdef __cplusplus
}
#endif
#ifdef MNG_LITEP_RUNTIME
static inline void mng_profile_cleanup(uint64_t* token) { mng_profile_end(*token); }
#define MNG_PROFILE_SCOPE(name) uint64_t mng_profile_token __attribute__((cleanup(mng_profile_cleanup))) = mng_profile_begin(name)
#else
#define MNG_PROFILE_SCOPE(name) ((void)0)
#endif
