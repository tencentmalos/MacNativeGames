// SPDX-License-Identifier: MIT
#pragma once
#include "lane_event_queue.h"

// Version 1: one data word followed by the original MONOTONIC timestamp.
// Row 0=null owner, 1=unknown owner, 2..257=immutable row slot 0..255.
// Bits: row[0:8], core+1[9:10], kind[11:12], svc[13:21],
// mask[22:24], signed HLE bits[25:56]. svc=256 means unknown (-1).
// Upper bits are reserved and must be zero. Consumers verify Count and Hash;
// this is an ordered packet protocol, never an adjacency-based causal join.
static inline int mng_lane_pack(const mng_lane_event *e,uint64_t *word) {
    if(e->row < -2 || e->row > 255 || e->core < -1 || e->core > 2 ||
       e->kind < MNG_LANE_OWNER || e->kind > MNG_LANE_EXIT ||
       e->svc < -1 || e->svc > 255 || (e->mask & ~7ull))return 0;
    unsigned row=e->row==-1?0:e->row==-2?1:(unsigned)e->row+2;
    *word=row | ((uint64_t)(e->core+1)<<9) | ((uint64_t)e->kind<<11) |
        ((uint64_t)(e->svc<0?256:e->svc)<<13) | (e->mask<<22) |
        ((uint64_t)(uint32_t)e->hle<<25);
    return 1;
}
static inline int mng_lane_unpack(uint64_t word,uint64_t at,mng_lane_event *e) {
    unsigned row=word&511,svc=(word>>13)&511;
    if((word>>57) || row>257 || svc>256)return 0;
    e->row=row==0?-1:row==1?-2:(int)row-2;e->core=(int)((word>>9)&3)-1;
    e->kind=(word>>11)&3;e->svc=svc==256?-1:(int)svc;e->mask=(word>>22)&7;
    e->hle=(int32_t)(uint32_t)(word>>25);e->at=at;
    return e->kind>=MNG_LANE_OWNER && e->kind<=MNG_LANE_EXIT;
}
static inline uint64_t mng_lane_hash_word(uint64_t hash,uint64_t word) {
    // FNV-1a over little-endian bytes, independent of host endian and padding.
    for(unsigned i=0;i<8;++i){hash^=(word>>(i*8))&255;hash*=1099511628211ull;}
    return hash;
}
#define MNG_LANE_HASH_SEED 14695981039346656037ull
