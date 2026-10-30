// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <time.h>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif
static inline uint64_t mng_host_clock_ns(int continuous) {
#if defined(__APPLE__)
    mach_timebase_info_data_t timebase;mach_timebase_info(&timebase);
    uint64_t ticks=continuous?mach_continuous_time():mach_absolute_time();
    return (uint64_t)((__uint128_t)ticks*timebase.numer/timebase.denom);
#else
    struct timespec value;
    if(clock_gettime(continuous?CLOCK_BOOTTIME:CLOCK_MONOTONIC,&value))return 0;
    return (uint64_t)value.tv_sec*1000000000ull+value.tv_nsec;
#endif
}

// Returns actual execution time charged to the calling thread, excluding sleep
// and descheduling. Zero means unavailable, not zero CPU work.
static inline uint64_t mng_thread_cpu_ns(void) {
    struct timespec value;
    if(clock_gettime(CLOCK_THREAD_CPUTIME_ID,&value))return 0;
    return (uint64_t)value.tv_sec*1000000000ull+value.tv_nsec;
}
