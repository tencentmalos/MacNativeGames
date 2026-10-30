// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <string.h>

// Caller serializes all access. No allocation, clocks, profiler or OS calls:
// transitions are also used from the existing guest preemption signal handler.
enum { MNG_LANE_CORES=3, MNG_LANE_ROWS=256, MNG_LANE_PAIRS=512 };
typedef struct {
    uint64_t id, held[MNG_LANE_CORES], held_previous, wait_ns, waits;
    uint64_t wait_since, cpu_previous, cpu_sample_previous;
    uint64_t cpu_ns, cpu_covered_ns, held_cpu_min_ns, held_cpu_max_ns;
    uint64_t held_offcpu_min_ns, held_offcpu_max_ns, cpu_unknown_ns;
    uint64_t tid;
    uint32_t handle;
    int wait_pair, svc, hle;
} mng_lane_row;
typedef struct {
    uint64_t waiter_id, ns, count, mask;
    uint32_t waiter, owners[MNG_LANE_CORES];
} mng_lane_pair;
typedef struct {
    mng_lane_row rows[MNG_LANE_ROWS];
    mng_lane_pair pairs[MNG_LANE_PAIRS];
    uint64_t since, previous, frame, frames, core_since[MNG_LANE_CORES];
    uint64_t core_held[MNG_LANE_CORES], core_idle[MNG_LANE_CORES];
    uint64_t row_overflow, pair_overflow, samples_failed, sampling_ns, sampling_max_ns;
    int owners[MNG_LANE_CORES];
    unsigned row_count, pair_count;
} mng_lane_accounting;

static inline void mng_lane_reset(mng_lane_accounting *s, uint64_t now) {
    memset(s,0,sizeof *s); s->since=s->previous=now;
    for(int c=0;c<MNG_LANE_CORES;++c){s->owners[c]=-1;s->core_since[c]=now;}
}
static inline int mng_lane_find(mng_lane_accounting *s,uint64_t id,uint32_t handle) {
    for(unsigned i=0;i<s->row_count;++i)if(s->rows[i].id==id)return (int)i;
    if(s->row_count==MNG_LANE_ROWS){++s->row_overflow;return -1;}
    int i=(int)s->row_count++;s->rows[i].id=id;s->rows[i].handle=handle;s->rows[i].wait_pair=-1;return i;
}
static inline uint64_t mng_lane_held(const mng_lane_row *r) {
    return r->held[0]+r->held[1]+r->held[2];
}
static inline void mng_lane_transfer(mng_lane_accounting *s,int c,int owner,uint64_t now) {
    uint64_t ns=now>=s->core_since[c]?now-s->core_since[c]:0;
    int old=s->owners[c];
    if(old>=0){s->core_held[c]+=ns;s->rows[old].held[c]+=ns;}
    else if(old==-1)s->core_idle[c]+=ns;
    // -2 is an untracked owner after capacity overflow, never idle.
    else s->core_held[c]+=ns;
    s->owners[c]=owner;s->core_since[c]=now;
}
static inline void mng_lane_wait_begin(mng_lane_accounting *s,int row,uint64_t mask,uint64_t now) {
    if(row<0)return;
    mng_lane_row *r=&s->rows[row];if(r->wait_since)return;
    uint32_t owners[MNG_LANE_CORES]={0};
    for(int c=0;c<MNG_LANE_CORES;++c)if((mask>>c&1) && s->owners[c]>=0)owners[c]=s->rows[s->owners[c]].handle;
    unsigned i=0;
    for(;i<s->pair_count;++i)if(s->pairs[i].waiter_id==r->id && s->pairs[i].mask==mask && !memcmp(owners,s->pairs[i].owners,sizeof owners))break;
    if(i==MNG_LANE_PAIRS){++s->pair_overflow;r->wait_pair=-1;}
    else {
        if(i==s->pair_count){++s->pair_count;s->pairs[i].waiter_id=r->id;s->pairs[i].waiter=r->handle;s->pairs[i].mask=mask;memcpy(s->pairs[i].owners,owners,sizeof owners);}
        r->wait_pair=(int)i;++s->pairs[i].count;
    }
    r->wait_since=now;++r->waits;
}
static inline void mng_lane_wait_tick(mng_lane_accounting *s,int row,uint64_t now,int complete) {
    if(row<0)return;
    mng_lane_row *r=&s->rows[row];if(!r->wait_since)return;
    uint64_t ns=now>=r->wait_since?now-r->wait_since:0;
    r->wait_ns+=ns;if(r->wait_pair>=0)s->pairs[r->wait_pair].ns+=ns;
    r->wait_since=complete?0:now;
}
// CPU is sampled once per successful Present, not at a core transition.
// A thread can consume host CPU while away from its virtual core. Therefore
// only bounds on held-CPU are defensible without scheduler/ownership overlap.
// skew bounds the differing CPU-clock sample and wall-accounting endpoints.
static inline void mng_lane_cpu(mng_lane_row *r,uint64_t wall,uint64_t cpu,uint64_t skew,int valid) {
    uint64_t held=mng_lane_held(r),dh=held-r->held_previous;r->held_previous=held;
    if(!valid){r->cpu_unknown_ns+=dh;return;}
    uint64_t away=wall>dh?wall-dh:0;
    uint64_t lo=cpu>away+skew?cpu-away-skew:0;
    if(lo>dh)lo=dh;
    uint64_t hi=cpu+skew<dh?cpu+skew:dh;
    r->cpu_ns+=cpu;r->cpu_covered_ns+=wall;
    r->held_cpu_min_ns+=lo;r->held_cpu_max_ns+=hi;
    r->held_offcpu_min_ns+=dh-hi;r->held_offcpu_max_ns+=dh-lo;
}
