// SPDX-License-Identifier: MIT
#include "../runtime/lane_accounting.h"
#include <assert.h>
#include <stdio.h>
static mng_lane_accounting s;
int main(void) {
    mng_lane_reset(&s,100);
    int a=mng_lane_find(&s,1,0x8001),b=mng_lane_find(&s,2,0x20004);
    mng_lane_transfer(&s,0,a,110);
    mng_lane_wait_begin(&s,b,1,115);
    mng_lane_transfer(&s,0,a,120); // Present splits an open ownership interval.
    mng_lane_wait_tick(&s,b,120,0);
    mng_lane_transfer(&s,0,b,125);mng_lane_wait_tick(&s,b,125,1);
    assert(s.core_idle[0]==10 && s.rows[a].held[0]==15);
    assert(s.rows[b].wait_ns==10 && s.rows[b].waits==1);
    assert(s.pairs[0].owners[0]==0x8001 && s.pairs[0].ns==10);
    mng_lane_transfer(&s,0,-1,140);
    mng_lane_transfer(&s,0,-1,150);
    assert(s.core_held[0]+s.core_idle[0]==50);
    // CPU outside the core must not be misreported as held CPU.
    mng_lane_cpu(&s.rows[a],50,35,0,1);
    assert(s.rows[a].held_cpu_min_ns==0 && s.rows[a].held_cpu_max_ns==15);
    assert(s.rows[a].held_offcpu_min_ns==0 && s.rows[a].held_offcpu_max_ns==15);
    // Wholly owned interval admits precise CPU attribution (zero skew).
    mng_lane_transfer(&s,1,a,150);mng_lane_transfer(&s,1,-1,200);
    mng_lane_cpu(&s.rows[a],50,30,0,1);
    assert(s.rows[a].held_offcpu_min_ns==20 && s.rows[a].held_offcpu_max_ns==35);
    // An invalid sample must not reuse an older CPU baseline.
    mng_lane_transfer(&s,1,a,200);mng_lane_transfer(&s,1,-1,210);
    mng_lane_cpu(&s.rows[a],10,0,0,0);assert(s.rows[a].cpu_unknown_ns==10);
    assert(mng_lane_find(&s,3,0x8001)!=a); // Handle reuse is keyed by thread id.
    for(int i=4;i<300;++i)mng_lane_find(&s,i,(uint32_t)i);
    assert(s.row_count==MNG_LANE_ROWS && s.row_overflow>0);
    mng_lane_transfer(&s,2,-2,210);mng_lane_transfer(&s,2,-1,220);
    assert(s.core_held[2]==10); // Untracked owner is not idle.
    mng_lane_reset(&s,500);a=mng_lane_find(&s,1,1);b=mng_lane_find(&s,2,2);
    for(int i=0;i<MNG_LANE_PAIRS+1;++i){
        s.rows[a].handle=(uint32_t)(i+1);mng_lane_transfer(&s,0,a,501+i*2);
        mng_lane_wait_begin(&s,b,1,501+i*2);mng_lane_wait_tick(&s,b,502+i*2,1);
    }
    assert(s.pair_count==MNG_LANE_PAIRS && s.pair_overflow==1);
    assert(s.rows[b].wait_ns==MNG_LANE_PAIRS+1);
    mng_lane_reset(&s,9000);assert(!s.row_count && !s.pair_count && !s.frames);
    puts("Lane interval clipping, CPU bounds, identity reuse and bounded overflow passed");
}
