// SPDX-License-Identifier: MIT
#pragma once
#ifdef MNG_LITEP_RUNTIME
#include "profile_c.h"
#else
#include <stdint.h>
#define MNG_PROFILE_SCOPE(name) ((void)0)
typedef struct { uint64_t id, generation; } mng_profile_flow;
static inline uint64_t mng_profile_begin(const char *name) { (void)name; return 0; }
static inline void mng_profile_end(uint64_t token) { (void)token; }
static inline mng_profile_flow mng_profile_post(const char *name) { (void)name; return (mng_profile_flow){0,0}; }
static inline uint64_t mng_profile_flow_begin(const char *name, mng_profile_flow flow) { (void)name; (void)flow; return 0; }
static inline void mng_profile_counter(const char *name, int64_t value) { (void)name; (void)value; }
#endif
