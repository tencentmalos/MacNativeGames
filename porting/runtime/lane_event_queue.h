// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <string.h>

// Multiple scheduler callers are serialized by G, giving this ring one logical
// producer. The Present thread is its sole consumer. No allocation or profiler
// calls occur on the producer (including guest preemption signals).
#ifndef MNG_LANE_EVENT_CAPACITY
#define MNG_LANE_EVENT_CAPACITY 65536
#endif
enum { MNG_LANE_OWNER=1, MNG_LANE_WAIT=2, MNG_LANE_EXIT=3 };
typedef struct {
    uint64_t at, mask;
    int kind, row, core, svc, hle;
} mng_lane_event;
typedef struct {
    mng_lane_event events[MNG_LANE_EVENT_CAPACITY];
    uint64_t write, read, overflow;
} mng_lane_event_queue;
static inline void mng_lane_events_reset(mng_lane_event_queue *q) {
    // Both producer and consumer must be quiescent at reset.
    __atomic_store_n(&q->write,0,__ATOMIC_RELAXED);
    __atomic_store_n(&q->read,0,__ATOMIC_RELAXED);
    __atomic_store_n(&q->overflow,0,__ATOMIC_RELAXED);
}
static inline int mng_lane_events_push(mng_lane_event_queue *q,mng_lane_event event) {
    uint64_t w=__atomic_load_n(&q->write,__ATOMIC_RELAXED);
    uint64_t r=__atomic_load_n(&q->read,__ATOMIC_ACQUIRE);
    if(w-r==MNG_LANE_EVENT_CAPACITY){__atomic_fetch_add(&q->overflow,1,__ATOMIC_RELAXED);return 0;}
    q->events[w%MNG_LANE_EVENT_CAPACITY]=event;
    __atomic_store_n(&q->write,w+1,__ATOMIC_RELEASE);return 1;
}
static inline uint64_t mng_lane_events_published(const mng_lane_event_queue *q) {
    return __atomic_load_n(&q->write,__ATOMIC_ACQUIRE);
}
static inline int mng_lane_events_peek(mng_lane_event_queue *q,uint64_t through,mng_lane_event *event) {
    uint64_t r=__atomic_load_n(&q->read,__ATOMIC_RELAXED);
    if(r==through)return 0;
    *event=q->events[r%MNG_LANE_EVENT_CAPACITY];return 1;
}
static inline void mng_lane_events_pop(mng_lane_event_queue *q) {
    uint64_t r=__atomic_load_n(&q->read,__ATOMIC_RELAXED);
    __atomic_store_n(&q->read,r+1,__ATOMIC_RELEASE);
}
