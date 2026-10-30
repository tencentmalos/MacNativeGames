// SPDX-License-Identifier: MIT
#define MNG_LANE_EVENT_CAPACITY 64
#include "../runtime/lane_event_queue.h"
#include "../runtime/lane_accounting.h"
#include <cassert>
#include <thread>
#include <atomic>
#include <cstdio>
static mng_lane_event_queue q;
int main(){
    // Busy scheduler: retain source boundaries, then replay exact ownership
    // transitions across both boundaries once a quiescent watermark is known.
    mng_lane_events_reset(&q);mng_lane_accounting s{};mng_lane_reset(&s,100);
    int a=mng_lane_find(&s,1,1),b=mng_lane_find(&s,2,2);
    mng_lane_events_push(&q,{110,0,MNG_LANE_OWNER,a,0,0,0});
    mng_lane_events_push(&q,{125,0,MNG_LANE_OWNER,b,0,0,0});
    mng_lane_events_push(&q,{145,0,MNG_LANE_OWNER,-1,0,0,0});
    auto published=mng_lane_events_published(&q);
    for(uint64_t boundary:{120,150}){
        mng_lane_event e{};
        while(mng_lane_events_peek(&q,published,&e) && e.at<=boundary){
            mng_lane_transfer(&s,e.core,e.row,e.at);mng_lane_events_pop(&q);
        }
        mng_lane_transfer(&s,0,s.owners[0],boundary);
        if(boundary==120)assert(s.rows[a].held[0]==10 && !s.rows[b].held[0]);
    }
    assert(s.rows[a].held[0]==15 && s.rows[b].held[0]==20 && s.core_idle[0]==15);
    // Full queue never blocks the guest; overflow is explicit and sticky.
    mng_lane_events_reset(&q);
    for(unsigned i=0;i<64;++i)assert(mng_lane_events_push(&q,{i,0,MNG_LANE_WAIT,0,0,0,0}));
    assert(!mng_lane_events_push(&q,{}));assert(q.overflow==1);
    mng_lane_events_reset(&q);
    // Concurrent wrap/reuse exercises the acquire/release slot ownership.
    constexpr uint64_t count=100000;std::atomic<bool> done{};
    std::thread producer([&]{for(uint64_t i=0;i<count;++i){
        while(__atomic_load_n(&q.write,__ATOMIC_RELAXED)-__atomic_load_n(&q.read,__ATOMIC_ACQUIRE)==64)std::this_thread::yield();
        assert(mng_lane_events_push(&q,{i,i^0xabc,MNG_LANE_OWNER,int(i%256),int(i%3),0,0}));
    }done=true;});
    uint64_t next=0;
    while(next<count){mng_lane_event e{};if(!mng_lane_events_peek(&q,mng_lane_events_published(&q),&e)){std::this_thread::yield();continue;}
        assert(e.at==next && e.mask==(next^0xabc) && e.row==int(next%256));++next;mng_lane_events_pop(&q);
    }
    producer.join();assert(done && q.overflow==0);
    puts("Lane event boundaries, overflow and concurrent wrap passed");
}
