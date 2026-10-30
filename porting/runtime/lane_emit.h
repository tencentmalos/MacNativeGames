// SPDX-License-Identifier: MIT
#pragma once
#include "lane_accounting.h"
#include "lane_event_queue.h"
#include "lane_packet.h"
#include "../profiler/profile_c.h"

// The SDK interns by pointer identity. Every name must be a stable literal;
// handles and TIDs are counter values in a row scope, never formatted names.
typedef struct {uint64_t token,count,rejected,hash;} mng_lane_packet;
static inline mng_lane_packet mng_lane_packet_begin(const mng_lane_accounting *s,
    uint64_t frame,uint64_t end,unsigned generation) {
    mng_lane_packet p={mng_profile_begin("Runtime.LanePacket"),0,0,MNG_LANE_HASH_SEED};
    if(!p.token)return p;
    mng_profile_counter("Lane.Packet.Version",1);
    mng_profile_counter("Lane.Packet.Generation",generation);
    mng_profile_counter("Lane.Packet.Frame",frame);
    mng_profile_counter("Lane.Packet.StartNs",s->previous);
    mng_profile_counter("Lane.Packet.EndNs",end);
    mng_profile_counter("Lane.Packet.Owner0",s->owners[0]);
    mng_profile_counter("Lane.Packet.Owner1",s->owners[1]);
    mng_profile_counter("Lane.Packet.Owner2",s->owners[2]);
    // Definitions are immutable within a lane generation, but replayed when
    // SDK recording resumes. Partial ring retention reports missing definitions.
    static uint64_t recording;static unsigned lane_generation,defined;
    if(recording!=p.token || lane_generation!=generation){recording=p.token;lane_generation=generation;defined=0;}
    for(;defined<s->row_count;++defined){const mng_lane_row *r=&s->rows[defined];
        uint64_t t=mng_profile_begin("Runtime.LaneThread");
        mng_profile_counter("Lane.Thread.Slot",defined);
        mng_profile_counter("Lane.Thread.Id",r->id);
        mng_profile_counter("Lane.Thread.Handle",r->handle);
        mng_profile_counter("Lane.Thread.HostTid",r->tid);
        mng_profile_end(t);
    }
    return p;
}
static inline void mng_lane_packet_event(mng_lane_packet *p,const mng_lane_event *e) {
    if(!p->token)return;
    uint64_t word;
    if(!mng_lane_pack(e,&word)){++p->rejected;return;}
    mng_profile_counter("Lane.Packet.Data",(int64_t)word);
    mng_profile_counter("Lane.Packet.TimeNs",(int64_t)e->at);
    p->hash=mng_lane_hash_word(mng_lane_hash_word(p->hash,word),e->at);++p->count;
}
static inline void mng_lane_packet_end(const mng_lane_packet *p) {
    if(!p->token)return;
    mng_profile_counter("Lane.Packet.Count",p->count);
    mng_profile_counter("Lane.Packet.Rejected",p->rejected);
    mng_profile_counter("Lane.Packet.Hash",(int64_t)p->hash);
    mng_profile_end(p->token);
}
static inline void mng_lane_emit(const mng_lane_accounting *s,uint64_t frame,uint64_t wall,
    uint64_t elapsed,uint64_t skipped,const uint64_t held_before[3],const uint64_t idle_before[3]) {
    static const char *const held_names[]={"Lane.Core0.HeldNs","Lane.Core1.HeldNs","Lane.Core2.HeldNs"};
    static const char *const idle_names[]={"Lane.Core0.IdleNs","Lane.Core1.IdleNs","Lane.Core2.IdleNs"};
    mng_profile_counter("Lane.NativeFrame",frame);
    mng_profile_counter("Lane.IntervalNs",wall);
    mng_profile_counter("Lane.SampleNs",elapsed);
    mng_profile_counter("Lane.SkippedPresents",skipped);
    for(int c=0;c<MNG_LANE_CORES;++c){
        mng_profile_counter(held_names[c],s->core_held[c]-held_before[c]);
        mng_profile_counter(idle_names[c],s->core_idle[c]-idle_before[c]);
    }
    for(unsigned i=0;i<s->row_count;++i){const mng_lane_row *r=&s->rows[i];
        if(!mng_lane_held(r) && !r->wait_ns)continue;
        uint64_t token=mng_profile_begin("Runtime.LaneRow");
        mng_profile_counter("Lane.Thread.Handle",r->handle);
        mng_profile_counter("Lane.Thread.Id",r->id);
        mng_profile_counter("Lane.Thread.HostTid",r->tid);
        mng_profile_counter("Lane.Thread.HeldNsTotal",mng_lane_held(r));
        mng_profile_counter("Lane.Thread.WaitNsTotal",r->wait_ns);
        mng_profile_counter("Lane.Thread.CpuNsTotal",r->cpu_ns);
        mng_profile_counter("Lane.Thread.HeldOffCpuMinNsTotal",r->held_offcpu_min_ns);
        mng_profile_counter("Lane.Thread.HeldOffCpuMaxNsTotal",r->held_offcpu_max_ns);
        mng_profile_counter("Lane.Thread.CpuUnknownHeldNsTotal",r->cpu_unknown_ns);
        mng_profile_end(token);
    }
}
