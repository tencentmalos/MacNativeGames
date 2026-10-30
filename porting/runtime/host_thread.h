// SPDX-License-Identifier: MIT
#pragma once
#include <pthread.h>
static inline void mng_name_thread(const char* name) {
#if defined(__APPLE__)
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(),name);
#endif
}
