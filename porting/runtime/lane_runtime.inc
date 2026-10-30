// SPDX-License-Identifier: MIT
#include "lane_emit.h"
// Present owns the accounting state behind g_lane_consumer. G only protects
// producer registration/events; Present never waits for G or reads thr_t.
enum { LANE_PENDING=16 };
typedef struct {uint64_t cpu,at;int ok;} lane_cpu_sample;
typedef struct {
    uint64_t frame,at,end,sampling_ns;
    unsigned count;
    lane_cpu_sample cpu[MNG_LANE_ROWS];
} lane_boundary;
static lane_boundary g_lane_pending[LANE_PENDING];
static unsigned g_lane_pending_head,g_lane_pending_count,g_lane_pending_max;
static uint64_t g_lane_dropped,g_lane_busy,g_lane_lag_max;
static uint64_t g_lane_retired[MNG_LANE_ROWS];
static void lane_release_clocks(void) { // both locks held; no readers
#ifndef __ANDROID__
    unsigned n=__atomic_load_n(&g_lane_thread_count,__ATOMIC_RELAXED);
    for(unsigned i=0;i<n;++i)if(g_lane_threads[i].clock_valid){
        mach_port_deallocate(mach_task_self(),g_lane_threads[i].port);g_lane_threads[i].clock_valid=0;
    }
#endif
}
static void lane_add_rows(unsigned n) { // consumer only
    while(g_lane.row_count<n){
        unsigned i=g_lane.row_count;const lane_thread *d=&g_lane_threads[i];
        int slot=mng_lane_find(&g_lane,d->id,d->handle);
        if(slot<0)break;
        g_lane.rows[slot].tid=d->tid;
    }
}
static void lane_cut(uint64_t now) {
    for(int c=0;c<NCORE;++c)mng_lane_transfer(&g_lane,c,g_lane.owners[c],now);
    for(unsigned i=0;i<g_lane.row_count;++i)mng_lane_wait_tick(&g_lane,(int)i,now,0);
}
int runtime_lane_control(int action,char *out,unsigned capacity) {
    if(pthread_mutex_trylock(&g_lane_consumer))return -1;
    if(action>=0){
        if(pthread_mutex_trylock(&G)){pthread_mutex_unlock(&g_lane_consumer);return -1;}
        if(action==2 || (action==1 && !atomic_load(&g_lane_enabled))){
            lane_release_clocks();
            __atomic_store_n(&g_lane_thread_count,0,__ATOMIC_RELAXED);
            __atomic_store_n(&g_lane_registration_overflow,0,__ATOMIC_RELAXED);
            __atomic_store_n(&g_lane_unregistered,0,__ATOMIC_RELAXED);
            mng_lane_events_reset(&g_lane_events);++g_lane_generation;
            uint64_t now=now_ns();mng_lane_reset(&g_lane,now);
            memset(g_lane_retired,0,sizeof g_lane_retired);
            g_lane_pending_head=g_lane_pending_count=g_lane_pending_max=0;
            g_lane_dropped=g_lane_busy=g_lane_lag_max=0;
            for(thr_t *t=g_all;t;t=t->all)if(t->started && !t->exited && __atomic_load_n(&t->mport,__ATOMIC_RELAXED))lane_register(t);
            lane_add_rows(__atomic_load_n(&g_lane_thread_count,__ATOMIC_RELAXED));
            for(int c=0;c<NCORE;++c)if(g_core_owner[c]){
                int row=lane_slot(g_core_owner[c]);mng_lane_transfer(&g_lane,c,row>=0?row:-2,now);
            }
            for(thr_t *t=g_core_wait;t;t=t->cnext)mng_lane_wait_begin(&g_lane,lane_slot(t),t->mask,now);
        }
        if(action==0){atomic_store(&g_lane_enabled,0);lane_release_clocks();}
        else if(action==1)atomic_store(&g_lane_enabled,1);
        pthread_mutex_unlock(&G);
    }
    int enabled=atomic_load(&g_lane_enabled);
    mng_lane_accounting copy=g_lane;
    copy.row_overflow=__atomic_load_n(&g_lane_registration_overflow,__ATOMIC_RELAXED);
    unsigned generation=g_lane_generation,pending=g_lane_pending_count,max_pending=g_lane_pending_max;
    uint64_t dropped=g_lane_dropped,busy=g_lane_busy,lag_max=g_lane_lag_max;
    uint64_t overflow=__atomic_load_n(&g_lane_events.overflow,__ATOMIC_RELAXED);
    uint64_t unregistered=__atomic_load_n(&g_lane_unregistered,__ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_lane_consumer);
    unsigned used=0;
#define LANE_WRITE(...) do {int n=snprintf(out+used,capacity-used,__VA_ARGS__);if(n<0 || (unsigned)n>=capacity-used)return -1;used+=(unsigned)n;}while(0)
    LANE_WRITE("schema=mng.lanes.v2\nlane_enabled=%d\ngeneration=%u\nframes=%llu\nlast_frame=%llu\nwall_ns=%llu\nskipped_presents=%llu\nrow_overflow=%llu\npair_overflow=%llu\ncpu_failures=%llu\nsampling_ns=%llu\nsampling_max_ns=%llu\nheld_cpu=bounded_not_exact\nwait_owners=observed_at_enqueue_not_causal_blockers\nhle_source=last_traced_call_not_current_call\npartial_tail=excluded\n",
        enabled,generation,(unsigned long long)copy.frames,(unsigned long long)copy.frame,
        (unsigned long long)(copy.previous-copy.since),(unsigned long long)dropped,
        (unsigned long long)copy.row_overflow,(unsigned long long)copy.pair_overflow,
        (unsigned long long)copy.samples_failed,(unsigned long long)copy.sampling_ns,(unsigned long long)copy.sampling_max_ns);
    LANE_WRITE("pending_presents=%u\nmax_pending_presents=%u\nwatermark_busy=%llu\nevent_overflow=%llu\nunregistered_threads=%llu\nboundary_lag_max_ns=%llu\nvalid=%d\n",pending,max_pending,
        (unsigned long long)busy,(unsigned long long)overflow,(unsigned long long)unregistered,(unsigned long long)lag_max,
        !dropped && !overflow && !copy.row_overflow && !copy.pair_overflow && !unregistered);
    for(int c=0;c<NCORE;++c)LANE_WRITE("core index=%d held_ns=%llu idle_ns=%llu\n",c,
        (unsigned long long)copy.core_held[c],(unsigned long long)copy.core_idle[c]);
    for(unsigned i=0;i<copy.row_count;++i){const mng_lane_row *r=&copy.rows[i];
        LANE_WRITE("lane handle=%#x id=%llu host_tid=%llu held_ns=%llu held0_ns=%llu held1_ns=%llu held2_ns=%llu wait_ns=%llu waits=%llu cpu_ns=%llu cpu_covered_ns=%llu held_cpu_min_ns=%llu held_cpu_max_ns=%llu held_offcpu_min_ns=%llu held_offcpu_max_ns=%llu cpu_unknown_held_ns=%llu last_svc=%d last_hle=%d\n",
            r->handle,(unsigned long long)r->id,(unsigned long long)r->tid,(unsigned long long)mng_lane_held(r),
            (unsigned long long)r->held[0],(unsigned long long)r->held[1],(unsigned long long)r->held[2],
            (unsigned long long)r->wait_ns,(unsigned long long)r->waits,(unsigned long long)r->cpu_ns,
            (unsigned long long)r->cpu_covered_ns,(unsigned long long)r->held_cpu_min_ns,(unsigned long long)r->held_cpu_max_ns,
            (unsigned long long)r->held_offcpu_min_ns,(unsigned long long)r->held_offcpu_max_ns,
            (unsigned long long)r->cpu_unknown_ns,r->svc,r->hle);
    }
    for(unsigned i=0;i<copy.pair_count;++i){const mng_lane_pair *p=&copy.pairs[i];
        LANE_WRITE("wait waiter=%#x id=%llu mask=%#llx owner0=%#x owner1=%#x owner2=%#x ns=%llu count=%llu\n",p->waiter,
            (unsigned long long)p->waiter_id,(unsigned long long)p->mask,p->owners[0],p->owners[1],p->owners[2],
            (unsigned long long)p->ns,(unsigned long long)p->count);
    }
#undef LANE_WRITE
    return (int)used;
}

static void lane_finish(const lane_boundary *f,uint64_t published,uint64_t watermark) {
    mng_lane_event e;
    mng_lane_packet packet=mng_lane_packet_begin(&g_lane,f->frame,f->at,g_lane_generation);
    while(mng_lane_events_peek(&g_lane_events,published,&e) && e.at<=f->at){
        mng_lane_packet_event(&packet,&e);
        if(e.row>=0 && (unsigned)e.row<g_lane.row_count){g_lane.rows[e.row].svc=e.svc;g_lane.rows[e.row].hle=e.hle;}
        if(e.kind==MNG_LANE_OWNER){
            mng_lane_transfer(&g_lane,e.core,e.row,e.at);
            if(e.row>=0)mng_lane_wait_tick(&g_lane,e.row,e.at,1);
        }else if(e.kind==MNG_LANE_WAIT)mng_lane_wait_begin(&g_lane,e.row,e.mask,e.at);
        mng_lane_events_pop(&g_lane_events);
    }
    mng_lane_packet_end(&packet);
    uint64_t held_before[NCORE],idle_before[NCORE];
    // Cumulative event totals already include transitions before this cut;
    // per-frame core deltas come from the previous completed boundary.
    static uint64_t previous_held[NCORE],previous_idle[NCORE];
    if(!g_lane.frames){memset(previous_held,0,sizeof previous_held);memset(previous_idle,0,sizeof previous_idle);}
    memcpy(held_before,previous_held,sizeof held_before);memcpy(idle_before,previous_idle,sizeof idle_before);
    lane_cut(f->at);
    uint64_t wall=f->at-g_lane.previous;
    for(unsigned i=0;i<g_lane.row_count;++i){
        mng_lane_row *r=&g_lane.rows[i];const lane_cpu_sample *sample=&f->cpu[i];
        // Exit is published before the kernel can reuse a TID. A quiescent
        // watermark after all clock reads lets us reject that identity race.
        int ok=i<f->count && sample->ok && (!g_lane_retired[i] || g_lane_retired[i]>sample->at);
        int valid=ok && r->cpu_sample_previous>=g_lane.previous && sample->cpu>=r->cpu_previous;
        uint64_t skew=sample->at>=f->at?sample->at-f->at:0;
        if(valid)skew+=r->cpu_sample_previous-g_lane.previous;
        mng_lane_cpu(r,wall,valid?sample->cpu-r->cpu_previous:0,skew+2000,valid);
        r->cpu_previous=ok?sample->cpu:0;r->cpu_sample_previous=ok?sample->at:0;
        if(i<f->count && !sample->ok && !g_lane_retired[i])++g_lane.samples_failed;
    }
    ++g_lane.frames;g_lane.frame=f->frame;g_lane.previous=f->at;
    g_lane.sampling_ns+=f->sampling_ns;if(f->sampling_ns>g_lane.sampling_max_ns)g_lane.sampling_max_ns=f->sampling_ns;
    uint64_t lag=watermark-f->end;if(lag>g_lane_lag_max)g_lane_lag_max=lag;
    uint64_t token=mng_profile_begin("Runtime.LaneFrame");
    if(token){
        mng_profile_counter("Lane.BoundaryNs",f->at);
#ifdef __ANDROID__
        // Explicit MONOTONIC/BOOTTIME bracket for the Perfetto sidecar. A
        // consumer must retain its uncertainty, not align by host wall time.
        struct timespec boot;
        uint64_t mono_before=now_ns();
        int clock_ok=!clock_gettime(CLOCK_BOOTTIME,&boot);
        uint64_t mono_after=now_ns();
        if(clock_ok){
            mng_profile_counter("Lane.ClockMonoBeforeNs",mono_before);
            mng_profile_counter("Lane.ClockBootNs",(uint64_t)boot.tv_sec*1000000000ull+boot.tv_nsec);
            mng_profile_counter("Lane.ClockMonoAfterNs",mono_after);
        }
#endif
        mng_profile_counter("Lane.BoundaryLagNs",lag);
        mng_profile_counter("Lane.EventOverflow",__atomic_load_n(&g_lane_events.overflow,__ATOMIC_RELAXED));
        mng_lane_emit(&g_lane,f->frame,wall,f->sampling_ns,g_lane_dropped,held_before,idle_before);
    }
    mng_profile_end(token);
    memcpy(previous_held,g_lane.core_held,sizeof previous_held);memcpy(previous_idle,g_lane.core_idle,sizeof previous_idle);
}
void runtime_lane_present(uint64_t frame) {
    if(!atomic_load_explicit(&g_lane_enabled,memory_order_relaxed))return;
    uint64_t token=mng_profile_begin("Runtime.LaneSample");
    // This lock is never acquired by a Guest or scheduler producer. Service
    // readers use try_lock and format their bounded copy outside it.
    pthread_mutex_lock(&g_lane_consumer);
    if(!atomic_load(&g_lane_enabled)){pthread_mutex_unlock(&g_lane_consumer);mng_profile_end(token);return;}
    if(g_lane_pending_count==LANE_PENDING)++g_lane_dropped;
    else {
        lane_boundary *f=&g_lane_pending[(g_lane_pending_head+g_lane_pending_count)%LANE_PENDING];
        f->frame=frame;f->at=now_ns();
        f->count=__atomic_load_n(&g_lane_thread_count,__ATOMIC_ACQUIRE);
        for(unsigned i=0;i<f->count;++i){const lane_thread *d=&g_lane_threads[i];lane_cpu_sample *sample=&f->cpu[i];
            sample->cpu=0;sample->ok=0;
#ifdef __ANDROID__
            struct timespec stamp;
            if(d->clock_valid && !clock_gettime(d->clock,&stamp)){
                sample->cpu=(uint64_t)stamp.tv_sec*1000000000ull+stamp.tv_nsec;sample->ok=1;
            }
#else
            thread_basic_info_data_t bi;mach_msg_type_number_t count=THREAD_BASIC_INFO_COUNT;
            if(d->clock_valid && thread_info(d->port,THREAD_BASIC_INFO,(thread_info_t)&bi,&count)==KERN_SUCCESS){
                sample->cpu=(uint64_t)(bi.user_time.seconds+bi.system_time.seconds)*1000000000ull+
                    (uint64_t)(bi.user_time.microseconds+bi.system_time.microseconds)*1000ull;sample->ok=1;
            }
#endif
            sample->at=now_ns();
        }
        f->end=now_ns();f->sampling_ns=f->end-f->at;
        ++g_lane_pending_count;if(g_lane_pending_count>g_lane_pending_max)g_lane_pending_max=g_lane_pending_count;
    }
    if(pthread_mutex_trylock(&G))++g_lane_busy;
    else {
        // Establish a publication watermark only. No CPU clocks, formatting,
        // table copies or event replay while holding the Guest scheduler lock.
        uint64_t published=mng_lane_events_published(&g_lane_events),watermark=now_ns();
        unsigned count=__atomic_load_n(&g_lane_thread_count,__ATOMIC_ACQUIRE);
        pthread_mutex_unlock(&G);
        lane_add_rows(count);
        uint64_t read=__atomic_load_n(&g_lane_events.read,__ATOMIC_RELAXED);
        for(uint64_t n=read;n<published;++n){const mng_lane_event *e=&g_lane_events.events[n%MNG_LANE_EVENT_CAPACITY];
            if(e->kind==MNG_LANE_EXIT && e->row>=0)g_lane_retired[e->row]=e->at;
        }
        while(g_lane_pending_count){
            lane_boundary *f=&g_lane_pending[g_lane_pending_head];
            if(f->end>watermark)break;
            lane_finish(f,published,watermark);
            g_lane_pending_head=(g_lane_pending_head+1)%LANE_PENDING;--g_lane_pending_count;
        }
    }
    pthread_mutex_unlock(&g_lane_consumer);mng_profile_end(token);
}
