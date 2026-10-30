/* Exercise the production condition path with a collecting profiler sink. */
#define MNG_LITEP_RUNTIME 1
#define GAME_BOTW 1
#include "../games/totk/runtime/threads.c"
#include <assert.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <unistd.h>

_Noreturn void runtime_fail(const char *message) { fprintf(stderr,"FAIL %s\n",message);abort(); }
uint64_t runtime_now_ns(void) { mach_timebase_info_data_t t;mach_timebase_info(&t);return (uint64_t)((__uint128_t)mach_absolute_time()*t.numer/t.denom); }
uint64_t runtime_hle_tsd_off;
void dm_thread_enter(uint64_t entry,uint64_t arg,uint64_t sp,uint64_t *context) { (void)entry;(void)arg;(void)sp;(void)context;abort(); }
void runtime_thread_init(void) { abort(); }
_Noreturn void runtime_thread_leave(const uint64_t *context,void *arg,void (*fn)(void *)) { (void)context;(void)arg;(void)fn;abort(); }
int runtime_guest_code_pc(uint64_t pc) { (void)pc;return 0; }
uint64_t mng_profile_begin(const char *name) { (void)name;return 1; }
void mng_profile_end(uint64_t token) { (void)token; }
void mng_profile_counter(const char *name,int64_t value) { (void)name;(void)value; }
static uint64_t epoch=1;
uint64_t mng_profile_guest_epoch(void) { return epoch; }
static struct { const char *name;uint64_t id;uint32_t waiter,peer;int64_t result; } events[32];
static unsigned event_count;
void mng_profile_guest_event(const char *name,uint64_t ep,uint64_t token,uint64_t id,uint32_t waiter,uint32_t peer,int64_t result) {
    if(!ep || ep!=epoch || !token)return;
    assert(event_count<32);events[event_count++]=(typeof(events[0])){name,id,waiter,peer,result};
}
uint64_t mng_profile_address_epoch(void) { return epoch; }
static struct {const char *name;uint64_t id,address;uint32_t waiter,peer;int64_t result;} address_events[32];
static unsigned address_count;
void mng_profile_address_event(const char *name,uint64_t ep,uint64_t token,uint64_t id,uint64_t address,uint32_t waiter,uint32_t peer,int64_t result) {
    if(!ep || ep!=epoch || !token)return;
    assert(address_count<32);address_events[address_count++]=(typeof(address_events[0])){name,id,address,waiter,peer,result};
}
static thr_t waiting,notifier;
static uint32_t mutex_word,condition_key;
static int64_t timeout=-1;
static void *wait_on_condition(void *unused) {
    (void)unused;pthread_setspecific(g_keythr,&waiting);
    uint64_t regs[32]={(uintptr_t)&mutex_word,(uintptr_t)&condition_key,waiting.handle,(uint64_t)timeout};
    assert(svc_thread(0x1c,regs));
    assert(regs[0]==(timeout==0?R_TIMEOUT:0));
    pthread_mutex_lock(&G);core_put(&waiting);pthread_mutex_unlock(&G);
    return NULL;
}
static uint32_t address_word;
static void *wait_on_address(void *unused) {
    (void)unused;pthread_setspecific(g_keythr,&waiting);
    uint64_t regs[32]={(uintptr_t)&address_word,2,0,(uint64_t)timeout};
    assert(svc_thread(0x34,regs));assert(regs[0]==(timeout==0?R_TIMEOUT:0));
    pthread_mutex_lock(&G);core_put(&waiting);pthread_mutex_unlock(&G);
    return NULL;
}
int main(void) {
    unsetenv("TOTK_FREE_CORES");unsetenv("TOTK_CORE_DIAG");
    pthread_key_t reserved;assert(!pthread_key_create(&reserved,NULL));assert(!pthread_key_create(&g_keythr,NULL));
    waiting=(thr_t){.handle=0x1101,.cur_core=-1,.mask=1,.core=0,.prio=44};
    notifier=(thr_t){.handle=0x1102,.cur_core=-1,.mask=1,.core=0,.prio=44};
    pthread_cond_init(&waiting.cv,NULL);pthread_setspecific(g_keythr,&notifier);
    mutex_word=waiting.handle;
    assert(!pthread_create(&waiting.pt,NULL,wait_on_condition,NULL));
    for(;;){pthread_mutex_lock(&G);int queued=waiting.wkind==2;pthread_mutex_unlock(&G);if(queued)break;usleep(100);}
    uint64_t signal_regs[32]={(uintptr_t)&condition_key,1};assert(svc_thread(0x1d,signal_regs));
    assert(!pthread_join(waiting.pt,NULL));
    assert(event_count==4);
    const char *names[]={"Guest.Condition.Enqueue","Guest.Condition.SelectedNotify","Guest.Condition.Wake","Guest.Condition.Resume"};
    for(unsigned i=0;i<4;++i){assert(!strcmp(events[i].name,names[i]));assert(events[i].id==events[0].id);assert(events[i].waiter==waiting.handle);}
    assert(events[1].peer==notifier.handle && !waiting.condition_token && !waiting.condition_epoch);
    event_count=0;timeout=0;mutex_word=waiting.handle;wait_on_condition(NULL);
    assert(event_count==2 && events[1].result==R_TIMEOUT);
    assert(!strcmp(events[0].name,"Guest.Condition.Enqueue") && !strcmp(events[1].name,"Guest.Condition.Resume"));
    event_count=0;epoch=0;mutex_word=waiting.handle;wait_on_condition(NULL);assert(!event_count);
    epoch=1;timeout=-1;pthread_setspecific(g_keythr,&notifier);
    assert(!pthread_create(&waiting.pt,NULL,wait_on_address,NULL));
    for(;;){pthread_mutex_lock(&G);int queued=waiting.wkind==3;pthread_mutex_unlock(&G);if(queued)break;usleep(100);}
    uint64_t notify_regs[32]={(uintptr_t)&address_word,0,0,1};assert(svc_thread(0x35,notify_regs));
    assert(!pthread_join(waiting.pt,NULL));assert(address_count==4);
    const char *address_names[]={"Guest.Address.Enqueue","Guest.Address.SelectedNotify","Guest.Address.Wake","Guest.Address.Resume"};
    for(unsigned i=0;i<4;++i){assert(!strcmp(address_events[i].name,address_names[i]));assert(address_events[i].id==address_events[0].id);assert(address_events[i].address==(uintptr_t)&address_word);assert(address_events[i].waiter==waiting.handle);}
    assert(address_events[1].peer==notifier.handle && !waiting.address_epoch && !waiting.address_token);
    uint64_t last_id=address_events[0].id;address_count=0;timeout=0;wait_on_address(NULL);
    assert(address_count==2 && address_events[1].result==R_TIMEOUT && address_events[0].id>last_id);
    assert(!strcmp(address_events[0].name,"Guest.Address.Enqueue") && !strcmp(address_events[1].name,"Guest.Address.Resume"));
    address_count=0;epoch=0;wait_on_address(NULL);assert(!address_count);
    puts("Condition/address enqueue, selected notifier, wake, resume, timeout and disabled markers passed");
}
