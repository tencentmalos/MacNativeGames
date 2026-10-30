// SPDX-License-Identifier: MIT
// Native NVN frontend. Command layouts and texture storage are supplied by the
// pinned MIT D2R renderer; no title-specific host or patch code is linked.
#include "nvn_vulkan.h"
#include "profiler.h"
#include "profile_c.h"
#include "../runtime/conservative_ranges.h"
#include "../runtime/ordered_executor.h"
#include "../runtime/submission_inputs.h"
#include "../runtime/host_thread.h"
#include "../runtime/host_clock.h"
#include "../runtime/host_import.h"
#include "render_record_vulkan.h"
#include "engine/xbg_game_internal.h"
#include "nvn_protos.h"
#include "../../android/third_party/d2r/sha256.h"
extern "C" {
#include "../tex/nvn_deswizzle.h"
}
#include "linear_texture.h"
#include "../../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_pace.h"
#include <chrono>
#include <thread>
#include <algorithm>
#include <atomic>
#include <array>
#include <cstring>
#include <cmath>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <type_traits>
#include <sys/mman.h>
#include <fcntl.h>
#if !defined(__APPLE__)
#include <sys/syscall.h>
#endif
#include <unistd.h>

static std::atomic<bool> custom_border_enabled{false};
extern "C" int mng_android_wait_display_tick(uint64_t*,int) MNG_OPTIONAL_IMPORT;
extern "C" int mng_vk_custom_border_enabled() { return custom_border_enabled.load(); }
void mng_nvn_set_custom_border_color(int enabled) { custom_border_enabled=enabled!=0; }

// MacNativeGames identifies Zelda shaders by control AND machine code. Many
// BOTW programs share identical control bytes while containing different code.
extern "C" int mng_nvn_shader_key(const void *control, uint64_t code, char out[17]) {
    if (!control || !out) return -1;
    // Handwritten self-test bundles have no guest machine code.
    if (!code) return xbg_shader_control_key(control,out);
    uint32_t declared,bytes;
    memcpy(&declared,static_cast<const uint8_t*>(control)+0x20,4);
    memcpy(&bytes,static_cast<const uint8_t*>(control)+0x704,4);
    if(!bytes || bytes>16u*1024*1024)return -1;
    const uint32_t length=declared>=0x7cc && declared<0x880?declared:0x880;
    mng_port_sha256_CTX digest;uint8_t sha[32];
    mng_port_sha256_Init(&digest);mng_port_sha256_Update(&digest,control,length);
    mng_port_sha256_Update(&digest,reinterpret_cast<const void*>(code),bytes);
    mng_port_sha256_Final(sha,&digest);
    for(unsigned i=0;i<8;++i)snprintf(out+2*i,3,"%02x",sha[i]);
    return 0;
}

#include "flush_trace.h"
#include "debugbus.h"
#include "../runtime/live_ranges.h"
#include "../runtime/frontlock_stats.h"
#include "../runtime/publication_queue.h"
#include "../runtime/texture_compare.h"
#include "runtime_policy.h"
#include <variant>
extern "C" uint32_t runtime_thread_current_handle() MNG_OPTIONAL_IMPORT;
namespace {
std::mutex flush_sample_mutex;
std::array<MngFlushSample,32> flush_samples{};
unsigned flush_sample_next{};
extern "C" unsigned mng_nvn_flush_samples(MngFlushSample* out,unsigned capacity){
    std::lock_guard lock(flush_sample_mutex);const auto n=std::min(capacity,unsigned(flush_samples.size()));
    std::copy_n(flush_samples.begin(),n,out);return n;
}
void note_flush(uint64_t address,uint64_t bytes){
    static const bool enabled=getenv("MNG_FLUSH_DIAGNOSTICS")!=nullptr;if(!enabled)return;
    spatial::LiteTrace::counter("NVN.Flush.Address",address);spatial::LiteTrace::counter("NVN.Flush.Bytes",bytes);
    std::lock_guard lock(flush_sample_mutex);
    for(auto &s:flush_samples)if(s.address==address && s.bytes==bytes){++s.count;return;}
    flush_samples[flush_sample_next++%flush_samples.size()]={address,bytes,1};
}
extern "C" void mng_nvn_invalidate_range(uint64_t,uint64_t);
std::recursive_mutex front_mutex;
std::atomic<const char*> front_holder_api{};
mng::port::FrontLockStats frontlock_stats;
int frontlock_control(int action,char* out,unsigned bytes){
    mng::port::FrontLockStats::Snapshot s;if(!frontlock_stats.control(action,s))return -1;
    int used=snprintf(out,bytes,"schema=mng.frontlock.v1\nfrontlock=%u\nepoch=%llu\nrows=%u\nother_count=%llu\nother_ns=%llu\nother_max_ns=%llu\nholder_semantics=observed_at_failed_try_lock;may_change_before_acquisition\n",
        unsigned(s.epoch&1),(unsigned long long)s.epoch,s.size,(unsigned long long)s.other_count,
        (unsigned long long)s.other_ns,(unsigned long long)s.other_max_ns);
    for(unsigned i=0;i<s.size && used>=0 && unsigned(used)<bytes;++i){const auto &r=s.rows[i];
        const int n=snprintf(out+used,bytes-unsigned(used),"row_%u=%s,%u,%s,%llu,%llu,%llu\n",i,
            r.waiter?r.waiter:"unknown",r.handle,r.holder?r.holder:"unknown",
            (unsigned long long)r.count,(unsigned long long)r.total_ns,(unsigned long long)r.max_ns);
        used=n<0?n:used+n;
    }
    return used<0 || unsigned(used)>=bytes?-1:used;
}
struct RetireTexture {tex_t texture{};};
struct RegisterTexture {dpool_t pool{};int id{};tex_t texture{};view_t view{};bool has_view{};};
struct RegisterSampler {dpool_t pool{};int id{};samp_par sampler{};};
using Publication=std::variant<RetireTexture,RegisterTexture,RegisterSampler>;
using PublicationQueue=mng::port::PublicationQueue<Publication,mng::vulkan::policy::publication_capacity>;
PublicationQueue publications;
std::atomic<uint64_t> publication_retired{},publication_textures{},publication_samplers{};
void drain_publications(){
    if(!publications.pending())return;
    spatial::LiteTraceScope scope("NVN.Publication.Drain");
    try{publications.drain([](Publication &operation){std::visit([](auto &value){
        using T=std::decay_t<decltype(value)>;
        if constexpr(std::is_same_v<T,RetireTexture>){xbg_legacy_tex_release(&value.texture);++publication_retired;}
        else if constexpr(std::is_same_v<T,RegisterTexture>){xbg_register_texture(&value.pool,value.id,&value.texture,value.has_view?&value.view:nullptr);++publication_textures;}
        else{xbg_register_sampler(&value.pool,value.id,&value.sampler);++publication_samplers;}
    },operation);});}
    catch(const std::exception &e){xbg_fatalf("Publication drain failed; packet retained: %s",e.what());}
    catch(...){xbg_fatalf("Publication drain failed; packet retained");}
}
using OrderedTicket=mng::port::OrderedExecutor::Ticket;
std::unique_ptr<mng::port::OrderedExecutor> translation_owner;
uint64_t ordered_owner_generation{};
OrderedTicket last_translation;
std::map<uint64_t,OrderedTicket> ordered_syncs;
uint64_t next_sync_id{};
std::map<const win_t*,std::array<OrderedTicket,4>> window_translation_tickets;
std::atomic<uint64_t> ordered_submits{},ordered_fallbacks{},ordered_capture_bytes{},ordered_drains{};
std::atomic<unsigned> ordered_fallback_reason{};
std::array<std::atomic<uint64_t>,10> ordered_fallback_counts{};
uint64_t ordered_report_submits{},ordered_report_outputs{};
mng::port::SubmissionSite ordered_last_failure_site;
std::array<uint64_t,128> ordered_opcode_counts{};
uint64_t ordered_opcode_overflow{};
mng::port::SubmissionSite ordered_failure_site;
std::atomic<uint64_t> ordered_waiters{};
std::array<uint64_t,7> ordered_input_counts{};
mng::port::SubmissionSite ordered_input_first;
bool ordered_input_first_valid{};
int ordered_inputs_control(int action,char* out,unsigned bytes){
    if(action!=-1)return snprintf(out,bytes,"error=read_only\n");
    std::unique_lock lock(front_mutex,std::try_to_lock);if(!lock.owns_lock())return -1;
    const auto &s=ordered_input_first;
    int used=snprintf(out,bytes,"schema=mng.ordered-inputs.v1\nfirst_valid=%u\nfirst_reason=%u\nfirst_opcode=%u\nfirst_handle=%u\nfirst_offset=%llu\nfirst_stage=%u\nfirst_binding=%u\nfirst_requested_bytes=%llu\nfirst_accumulated_bytes=%llu\nstage_domain=engine_stage;4294967295_unknown\n",
        unsigned(ordered_input_first_valid),unsigned(s.input_failure),s.opcode,s.handle,(unsigned long long)s.offset,
        s.stage,s.binding,(unsigned long long)s.requested_bytes,(unsigned long long)s.accumulated_bytes);
    for(unsigned i=1;i<ordered_input_counts.size() && used>=0 && unsigned(used)<bytes;++i){
        const int n=snprintf(out+used,bytes-unsigned(used),"reason_%u=%llu\n",i,(unsigned long long)ordered_input_counts[i]);used=n<0?n:used+n;}
    return used<0 || unsigned(used)>=bytes?-1:used;
}
extern "C" int mng_capture_present(const tex_t*,mng::port::SubmissionInputs*);
extern "C" void mng_execute_present(const win_t*,const tex_t*,const mng::port::SubmissionInputs*);
extern "C" int mng_capture_submission(int,const uint64_t*,mng::port::SubmissionInputs*,mng::port::SubmissionSite*);
extern "C" uint64_t mng_execute_submission(int,const uint64_t*,const mng::port::SubmissionInputs*);
// FrontLock rechecks the queue tail with publication excluded after every wait.
// The renderer worker never takes front_mutex, including its callbacks.
void drain_translation(){
    if(translation_owner){spatial::LiteTraceScope scope("NVN.Ordered.Drain");
        ++ordered_drains;translation_owner->drain();}
}
// SyncWait consumes a value snapshot; it never holds this mutex while waiting
// for the renderer, GPU, or a guest virtual core. Queue operations retain their
// existing front -> engine ordering and publish the snapshot afterwards.
std::mutex sync_mutex;
mng::port::ConservativeRanges mapped_gpu_ranges;
std::atomic<bool> live_range_flush{false},ordered_flush_fallback{true},image_flush{false};
// Mirror the engine range policy without taking its translation lock. A disabled
// range policy must retain the full queue-completion diagnostic path.
std::atomic<bool> invalidate_fast{false},range_invalidate_enabled{true};
std::atomic<uint64_t> invalidate_clear{},invalidate_conflict{},invalidate_legacy{},invalidate_unknown{};
extern "C" void mng_nvn_range_invalidate_mode(int enabled){range_invalidate_enabled.store(enabled!=0,std::memory_order_release);}
int control_invalidate(int enabled,char* out,unsigned bytes){
    if(enabled>=0)invalidate_fast.store(enabled!=0,std::memory_order_release);
    return snprintf(out,bytes,"invalidate_fast=%u\ninvalidate_effective=%u\ninvalidate_clear=%llu\ninvalidate_conflict=%llu\ninvalidate_legacy=%llu\ninvalidate_unknown=%llu\n",
        unsigned(invalidate_fast.load()),unsigned(invalidate_fast.load() && range_invalidate_enabled.load() && !ordered_flush_fallback.load()),
        (unsigned long long)invalidate_clear.load(),(unsigned long long)invalidate_conflict.load(),
        (unsigned long long)invalidate_legacy.load(),(unsigned long long)invalidate_unknown.load());
}
std::atomic<uint64_t> flush_image_fast{};
int control_image_flush(int enabled,char* out,unsigned bytes){
    if(enabled>=0)image_flush.store(enabled!=0,std::memory_order_release);
    return snprintf(out,bytes,"image_flush=%u\nflush_image_fast=%llu\n",unsigned(image_flush.load()),(unsigned long long)flush_image_fast.load());
}
std::atomic<uint64_t> flush_live_fast{},flush_live_conflict{},flush_legacy{},flush_unknown{};
extern "C" void mng_nvn_live_flush_mode(int enabled){live_range_flush=enabled!=0;}
extern "C" int mng_nvn_live_flush_status(char *out,unsigned bytes){
    const auto stats=mng::port::flush_dependencies().stats();
    return snprintf(out,bytes,"live_range_flush=%u\nflush_live_fast=%llu\nflush_live_conflict=%llu\nflush_legacy=%llu\nflush_unknown=%llu\nflush_live_ranges=%zu\nflush_unknown_ranges=%llu\nflush_ordered_fallback=%u\n",
        unsigned(live_range_flush.load()),(unsigned long long)flush_live_fast.load(),
        (unsigned long long)flush_live_conflict.load(),(unsigned long long)flush_legacy.load(),
        (unsigned long long)flush_unknown.load(),stats.ranges,(unsigned long long)stats.unknown,
        unsigned(ordered_flush_fallback.load()));
}
extern "C" void mng_nvn_note_gpu_range(uint64_t address,uint64_t bytes) {
    mapped_gpu_ranges.include(address,bytes);
}
extern "C" int mng_nvn_ordered_status(char *out,unsigned bytes){
    std::unique_lock lock(front_mutex,std::try_to_lock);
    if(!lock.owns_lock())return snprintf(out,bytes,"ordered_status=busy\n");
    const auto stats=translation_owner?translation_owner->stats():mng::port::OrderedExecutor::Stats{};
    int used=snprintf(out,bytes,"ordered_translation=%u\nordered_submits=%llu\nordered_fallbacks=%llu\nordered_fallback_reason=%u\nordered_capture_bytes=%llu\nordered_drains=%llu\nordered_admitted=%llu\nordered_completed=%llu\nordered_pending=%llu\nordered_bytes=%llu\nordered_peak_bytes=%llu\n",
        unsigned(bool(translation_owner)),(unsigned long long)ordered_submits.load(),
        (unsigned long long)ordered_fallbacks.load(),ordered_fallback_reason.load(),
        (unsigned long long)ordered_capture_bytes.load(),(unsigned long long)ordered_drains.load(),
        (unsigned long long)stats.admitted,(unsigned long long)stats.completed,
        (unsigned long long)stats.pending,(unsigned long long)stats.bytes,(unsigned long long)stats.peak_bytes);
    for(unsigned i=1;i<ordered_fallback_counts.size() && used>=0 && unsigned(used)<bytes;++i){
        const int n=snprintf(out+used,bytes-unsigned(used),"ordered_fallback_%u=%llu\n",i,(unsigned long long)ordered_fallback_counts[i].load());
        if(n<0)return n;used+=n;
    }
    auto append=[&](const char *format,auto... values){
        if(used>=0 && unsigned(used)<bytes){const int n=snprintf(out+used,bytes-unsigned(used),format,values...);used=n<0?n:used+n;}
    };
    append("ordered_waiters=%llu\nordered_first_opcode=%u\nordered_first_handle=%u\nordered_first_offset=%llu\nordered_opcode_overflow=%llu\n",
        (unsigned long long)ordered_waiters.load(),ordered_failure_site.opcode,ordered_failure_site.handle,
        (unsigned long long)ordered_failure_site.offset,(unsigned long long)ordered_opcode_overflow);
    append("ordered_report_submits=%llu\nordered_report_outputs=%llu\nordered_last_opcode=%u\nordered_last_handle=%u\nordered_last_offset=%llu\n",
        (unsigned long long)ordered_report_submits,(unsigned long long)ordered_report_outputs,
        ordered_last_failure_site.opcode,ordered_last_failure_site.handle,(unsigned long long)ordered_last_failure_site.offset);
    unsigned listed=0,total=0;
    for(unsigned i=0;i<ordered_opcode_counts.size();++i)if(ordered_opcode_counts[i]){
        ++total;if(listed++<8)append("ordered_opcode_%u=%llu\n",i,(unsigned long long)ordered_opcode_counts[i]);
    }
    append("ordered_opcode_rows_omitted=%u\n",total>8?total-8:0);
    return used;
}
// Recording only owns CPU data. Never call the Vulkan engine while holding
// command_mutex: translation takes engine -> command for its range check.
// Submit takes front -> command only to pin input, then drops command before
// entering the engine. Queries use object_mutex independently.
std::recursive_mutex command_mutex;
// Stable object lookup is independent of command translation. A getter must
// not park a guest virtual core behind an entire synchronous Vulkan submit.
// Never hold this mutex while acquiring front_mutex or the engine mutex.
std::mutex object_mutex;
std::map<const win_t*,uint64_t> window_display_ticks;
std::map<const win_t*,std::array<uint64_t,4>> window_present_tickets;
extern "C" uint64_t mng_nvn_present_ticket();
extern "C" void mng_nvn_wait_present(uint64_t);
int (*core_away)();void (*core_back)(int*);
// Declare before frontend locks: reacquire the guest core only after all host
// locks have been released, matching MacNative's NVN_BLOCKING contract.
struct HostWait {
    int away=core_away?core_away():0;
    ~HostWait(){if(core_back)core_back(&away);}
};
extern "C" void mng_nvn_flush();
bool ready;
void *(*host_wrapper)(void*,const char*);
std::map<std::string, void *> &functions() {
    static std::map<std::string, void *> table;
    return table;
}
struct Register {
    Register(const char *name, void *address) { functions().emplace(name, address); }
};
#define API(name, result, ...)                                                                     \
    static result name(__VA_ARGS__);                                                               \
    [[maybe_unused]] static const NVNT_##name signature_##name = name;                             \
    static Register register_##name(#name, reinterpret_cast<void *>(name));                        \
    static result name(__VA_ARGS__)
struct FrontLock {
    static inline thread_local unsigned depth{};
    uint64_t profile{},api_profile{};
    int away{};void (*back)(int*){};
    explicit FrontLock(const char* api,bool drain=true) {
        if(!depth){api_profile=mng_profile_begin(api);back=core_back;away=core_away?core_away():0;}
        std::unique_lock held(front_mutex,std::defer_lock);
        try {
            if(!held.try_lock()){
                const auto token=frontlock_stats.token();
                const auto *holder=token?front_holder_api.load(std::memory_order_acquire):nullptr;
                const auto handle=token && runtime_thread_current_handle?runtime_thread_current_handle():0;
                const auto begin=token?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
                {spatial::LiteTraceScope waiting("NVN.FrontLock.Wait");held.lock();}
                if(token)frontlock_stats.record(token,api,handle,holder,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count());
            }
            if(!depth){front_holder_api.store(api,std::memory_order_release);drain_publications();}
            if(drain && !depth && translation_owner){
                spatial::LiteTraceScope waiting("NVN.Ordered.Drain");++ordered_drains;
                const auto generation=ordered_owner_generation;
                for(;;){
                    if(generation!=ordered_owner_generation)throw std::runtime_error("Renderer ownership changed during drain");
                    auto tail=translation_owner?translation_owner->tail():OrderedTicket{};
                    if(tail.wait(0))break;
                    ++ordered_waiters;front_holder_api.store(nullptr,std::memory_order_release);held.unlock();
                    try{tail.wait();}catch(...){--ordered_waiters;throw;}
                    --ordered_waiters;held.lock();front_holder_api.store(api,std::memory_order_release);
                    // Publication or shutdown may have occurred while unlocked.
                }
            }else if(drain && depth)drain_translation();
            if(depth++==0)profile=mng_profile_begin("NVN.FrontLock.Hold");
            held.release();
        }catch(...){
            if(held.owns_lock()){if(!depth)front_holder_api.store(nullptr,std::memory_order_release);held.unlock();}
            mng_profile_end(api_profile);if(back)back(&away);throw;
        }
    }
    void release(){
        if(--depth == 0){front_holder_api.store(nullptr,std::memory_order_release);mng_profile_end(profile);}
        front_mutex.unlock();mng_profile_end(api_profile);
        if(back)back(&away);
    }
    ~FrontLock(){release();}
};
// Control takes no blocking front lock on the Android service thread.
int control_publication(int enabled,char* out,unsigned bytes){
    if(enabled>=0){
        std::unique_lock lock(front_mutex,std::try_to_lock);if(!lock.owns_lock())return -1;
        if(enabled && translation_owner)return snprintf(out,bytes,"error=publication_ordered_translation\n");
        if(!publications.try_enable(enabled!=0))return -1;
        if(!enabled)drain_publications();
    }
    PublicationQueue::Stats s;if(!publications.snapshot(s))return -1;
    return snprintf(out,bytes,"schema=mng.publication.v1\npublication_queue=%u\npublication_failed=%u\nenqueued=%llu\ndrained=%llu\noverflow=%llu\nmax_depth=%llu\npending=%llu\nin_flight=%llu\ndrain_ns_total=%llu\ndrain_ns_max=%llu\ndrain_calls=%llu\nretired=%llu\nregistered_textures=%llu\nregistered_samplers=%llu\ncapacity=%zu\n",
        unsigned(s.enabled),unsigned(s.failed),(unsigned long long)s.enqueued,(unsigned long long)s.drained,
        (unsigned long long)s.overflow,(unsigned long long)s.max_depth,(unsigned long long)s.pending,(unsigned long long)s.in_flight,
        (unsigned long long)s.drain_ns_total,(unsigned long long)s.drain_ns_max,(unsigned long long)s.drain_calls,
        (unsigned long long)publication_retired.load(),(unsigned long long)publication_textures.load(),
        (unsigned long long)publication_samplers.load(),mng::vulkan::policy::publication_capacity);
}
#define LOCK FrontLock guard(__func__)
#define PUBLISH_LOCK FrontLock guard(__func__,false)
#define COMMAND_LOCK std::lock_guard guard(command_mutex)
void need(bool condition, const char *message) {
    if (!condition)
        xbg_fatalf("Native NVN: %s", message);
}
// Keep value-only objects in guest storage, as in the Mac frontend. Owning
// objects still use stable handles; engine pointers never refer to guest values.
struct Buffer {
    const NVNmemoryPool *pool;
    ptrdiff_t offset;
    size_t bytes;
};
template<class T> constexpr bool inline_object =
    std::is_same_v<T,dev_builder_t> || std::is_same_v<T,pool_builder_t> ||
    std::is_same_v<T,queue_builder_t> || std::is_same_v<T,win_builder_t> ||
    std::is_same_v<T,ev_builder_t> || std::is_same_v<T,samp_builder_t> ||
    std::is_same_v<T,samp_t> || std::is_same_v<T,Buffer> || std::is_same_v<T,sync_t>;
static_assert(sizeof(dev_builder_t)<=8 && sizeof(pool_builder_t)<=64);
static_assert(sizeof(queue_builder_t)<=64 && sizeof(win_builder_t)<=64);
static_assert(sizeof(ev_builder_t)<=32 && sizeof(samp_builder_t)<=96);
static_assert(sizeof(samp_t)<=48 && sizeof(Buffer)<=48);
static_assert(sizeof(sync_t)<=64);
struct Object {
    virtual ~Object() = default;
};
uint64_t object_counts[8]{};
template<class T> constexpr unsigned object_kind = std::is_same_v<T,cmd_t>?0:
    std::is_same_v<T,pool_t>?1:std::is_same_v<T,dpool_t>?2:std::is_same_v<T,prog_t>?3:
    std::is_same_v<T,sync_t>?4:std::is_same_v<T,queue_t>?5:std::is_same_v<T,win_t>?6:7;
template <class T> struct Box final : Object {
    Box(){++object_counts[object_kind<T>];}
    ~Box() override{--object_counts[object_kind<T>];}
    T value{};
};
std::map<uint64_t, std::unique_ptr<Object>> objects;
uint64_t next_object = 0x4d4e470000000000ull;
xbg_game_config host_config{};
int (*memory_validator)(uint64_t,size_t);
struct PoolBacking {int fd=-1;bool virtual_range=false;std::shared_ptr<mng::port::ReportPool> report_pool;};
std::map<const pool_t*,PoolBacking> pool_backings;
extern "C" bool mng_nvn_capture_report_output(uint64_t address,mng::port::SubmissionInputs *input){
    need(FrontLock::depth!=0,"Report capture requires frontend ownership");
    static_assert(sizeof(NVNcounterData)==mng::port::SubmissionInputs::report_bytes);
    constexpr auto bytes=sizeof(NVNcounterData);
    if(!address || (address&15) || bytes>UINT64_MAX-address)return false;
    std::shared_ptr<const mng::port::ReportPool> owner;
    for(const auto &[pool,backing]:pool_backings){
        const auto begin=uint64_t(pool->mem);
        if(begin<address+bytes && address<begin+pool->size){
            // Virtual/physical aliases require backing-offset identity, not VA comparison.
            if(owner || !backing.report_pool || !backing.report_pool->contains(address,bytes))return false;
            owner=backing.report_pool;
        }
    }
    return input->add_report(address,std::move(owner));
}
template <class T, class U> T &as(U *p, const char *caller = __builtin_FUNCTION()) {
    if constexpr (inline_object<T>) {
        static_assert(sizeof(T)<=sizeof(U));
        need(p,"Null inline NVN object");
        return *reinterpret_cast<T*>(p);
    }
    std::lock_guard guard(object_mutex);
    static_assert(sizeof(U) >= 8);
    if (!p) xbg_fatalf("Null NVN object in %s type=%s", caller, typeid(T).name());
    uint64_t id = 0;
    memcpy(&id, p, 8);
    auto it = objects.find(id);
    if (it != objects.end())
        if (auto *box = dynamic_cast<Box<T> *>(it->second.get()))
            return box->value;
    need(objects.size() < 1048576, "Native object retention limit");
    auto box = std::make_unique<Box<T>>();
    auto *value = &box->value;
    id = ++next_object;
    objects.emplace(id, std::move(box));
    memcpy(p, &id, 8);
    return *value;
}
template <class T, class U> const T &as(const U *p, const char *caller = __builtin_FUNCTION()) {
    if constexpr (inline_object<T>) {
        static_assert(sizeof(T)<=sizeof(U));
        need(p,"Null inline NVN object");
        return *reinterpret_cast<const T*>(p);
    }
    std::lock_guard guard(object_mutex);
    static_assert(sizeof(U) >= 8);
    if (!p) xbg_fatalf("Null NVN object in %s type=%s", caller, typeid(T).name());
    uint64_t id = 0;
    memcpy(&id, p, 8);
    auto it = objects.find(id);
    if(it==objects.end())xbg_fatalf("Uninitialized NVN object %p cookie=%#llx type=%s",p,(unsigned long long)id,typeid(T).name());
    auto *box = dynamic_cast<Box<T> *>(it->second.get());
    need(box != nullptr, "Wrong object kind");
    return box->value;
}
bool range(uint64_t size, int64_t offset, uint64_t count) {
    return offset >= 0 && uint64_t(offset) <= size && count <= size - uint64_t(offset);
}
// Recordings and replay state own their immutable texture metadata. The cache
// only keeps unreferenced entries until the next bounded sweep. This follows
// the lifetime roots documented by MacNativeGames' MIT tex_pin implementation.
using TextureRefs = std::map<uint64_t, std::shared_ptr<const tex_t>>;
std::mutex texture_snapshot_mutex;
TextureRefs texture_snapshots, replay_texture_refs;
uint64_t texture_snapshot_creations{};
void retain_recorded_texture(NVNcommandBuffer*, const std::shared_ptr<const tex_t>&);
void invalidate_pool_textures(const pool_t *pool);
// The compact texture follows MacNative's by-value lifetime model. Reordering
// the engine fields removes padding and excludes its snapshot pointer. Never
// write the wider D2R tex_t into the 128-byte native Zelda object.
struct CompactTexture {
    uint32_t magic;
    nvn_texdesc d;
    uint8_t has_layout, layout[8];
    nvn_texlayout L;
    uint8_t *mem;
    pool_t *pool;
    int64_t off;
    uint64_t serial;
};
static_assert(sizeof(CompactTexture)<=128 && sizeof(CompactTexture)<=sizeof(NVNtexture));
std::atomic<uint64_t> texture_initializations{},texture_finalizations{};
extern "C" void mng_nvn_texture_counters(uint64_t* initialized,uint64_t* finalized){
    *initialized=texture_initializations.load(std::memory_order_relaxed);
    *finalized=texture_finalizations.load(std::memory_order_relaxed);
}
constexpr uint64_t swizzle_mask=0xfff;
uint64_t pack_swizzle(const std::array<uint8_t,4>& c) {
    return uint64_t(c[0]) | (uint64_t(c[1])<<3) | (uint64_t(c[2])<<6) | (uint64_t(c[3])<<9);
}
tex_t read_texture(const NVNtexture *p) {
    need(p,"Null texture");CompactTexture c{};memcpy(&c,p,sizeof c);
    tex_t t{};t.magic=c.magic;t.d=c.d;t.L=c.L;t.mem=c.mem;t.pool=c.pool;
    t.off=c.off;t.serial=c.serial;t.has_layout=c.has_layout;memcpy(t.layout,c.layout,8);
    return t;
}
void write_texture(NVNtexture *p,const tex_t& t) {
    need(p,"Null texture");CompactTexture c{};
    c.magic=t.magic;c.d=t.d;c.L=t.L;c.mem=t.mem;c.pool=t.pool;
    c.off=t.off;c.serial=t.serial;c.has_layout=t.has_layout;memcpy(c.layout,t.layout,8);
    memcpy(p,&c,sizeof c);
}
// Builders are by-value, non-owning state and have no Finalize operation.
// Keeping them in the object registry leaked every zeroed stack temporary.
struct TextureBuilderState { tex_builder_t value; std::array<uint8_t,4> swizzle; };
static_assert(sizeof(TextureBuilderState)<=128);
static_assert(sizeof(TextureBuilderState)<=sizeof(NVNtextureBuilder));
TextureBuilderState &texture_builder(NVNtextureBuilder *b) {
    need(b,"Null texture builder");return *reinterpret_cast<TextureBuilderState*>(b);
}
const TextureBuilderState &texture_builder(const NVNtextureBuilder *b) {
    need(b,"Null texture builder");return *reinterpret_cast<const TextureBuilderState*>(b);
}
const tex_t *snapshot(NVNcommandBuffer *command, const NVNtexture *texture, const char *operation) {
    auto t = read_texture(texture);
    if(t.magic != MAGIC('T', 'E', 'X', 0))
        xbg_fatalf("Invalid texture in %s: guest=%p magic=%#x serial=%llu size=%ux%u",
            operation,texture,t.magic,(unsigned long long)t.serial,t.d.width,t.d.height);
    need(t.L.bpb && t.L.size,"Unformatted texture used by a GPU command");
    std::lock_guard lock(texture_snapshot_mutex);
    if (!texture_snapshots.count(t.serial) && (++texture_snapshot_creations % 1024 == 0 || texture_snapshots.size() >= 65536)) {
        for (auto i=texture_snapshots.begin();i!=texture_snapshots.end();)
            if (i->second.use_count()==1) i=texture_snapshots.erase(i); else ++i;
    }
    auto &copy = texture_snapshots[t.serial];
    if (!copy) {
        need(texture_snapshots.size() <= 65536, "Live texture snapshot limit");
        copy = std::make_shared<const tex_t>(t);
    }
    retain_recorded_texture(command, copy);
    return copy.get();
}
// By-value texture views fit the 16-byte storage used by the native Zelda
// frontend. This is our representation; convert to the engine's wider view.
struct CompactView {
    uint16_t level, levels, layer, layers, format;
    uint8_t target, flags;
    uint32_t reserved;
};
static_assert(sizeof(CompactView) == 16);
CompactView read_view(const NVNtextureView *p) {
    CompactView v{};
    if (p)
        memcpy(&v, p, sizeof(v));
    return v;
}
void write_view(NVNtextureView *p, const CompactView &v) {
    need(p, "Null view");
    memcpy(p, &v, sizeof(v));
}
view_t translate_view(const NVNtextureView *p, const NVNtexture *texture=nullptr, bool attachment=false) {
    auto v = read_view(p);
    // NVN zero counts mean the remaining subresources, as also implemented by
    // the Mac backend. Resolve them only when a texture identity is available;
    // getters keep the original zero-valued view fields.
    if(texture) {
        const auto& t=read_texture(texture);
        const auto base=(v.flags&1)?v.level:0;
        need(base<t.d.levels,"View base mip outside texture");
        const uint32_t layers=attachment && t.d.target==NVN_TEXTURE_TARGET_3D?
            std::max(1u,t.d.depth>>base):t.L.layers;
        if((v.flags&1) && !v.levels)v.levels=t.d.levels-base;
        if((v.flags&2) && !v.layers) {
            need(v.layer<layers && layers-v.layer<=65535,"View remaining layer range");
            v.layers=layers-v.layer;
        }
    }
    return {uint8_t(v.flags & 1),
            uint8_t((v.flags >> 1) & 1),
            uint8_t((v.flags >> 2) & 1),
            uint8_t((v.flags >> 3) & 1),
            v.level,
            v.levels,
            v.layer,
            v.layers,
            v.format,
            v.target};
}
API(nvnTextureViewSetDefaults, void, NVNtextureView *v) { write_view(v, {}); }
API(nvnTextureViewSetSwizzle, void, NVNtextureView *v, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle b, NVNtextureSwizzle a) {
    need(unsigned(r)<=5 && unsigned(g)<=5 && unsigned(b)<=5 && unsigned(a)<=5,"View swizzle range");
    auto state=read_view(v);state.reserved=uint32_t(r)|(uint32_t(g)<<8)|(uint32_t(b)<<16)|(uint32_t(a)<<24);state.flags|=16;write_view(v,state);
}
API(nvnTextureViewSetDepthStencilMode, void, NVNtextureView *v, NVNtextureDepthStencilMode mode) {
    need(mode==NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH,"Stencil-plane view unsupported");auto state=read_view(v);state.flags|=32;write_view(v,state);
}
API(nvnTextureViewGetSwizzle, NVNboolean, const NVNtextureView *v, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *b, NVNtextureSwizzle *a) {
    auto state=read_view(v);if(!(state.flags&16))return 0;
    *r=NVNtextureSwizzle(state.reserved&255);*g=NVNtextureSwizzle((state.reserved>>8)&255);*b=NVNtextureSwizzle((state.reserved>>16)&255);*a=NVNtextureSwizzle(state.reserved>>24);return 1;
}
API(nvnTextureViewSetLevels, void, NVNtextureView *v, int first, int count) {
    need(first >= 0 && first <= 65535 && count >= 0 && count <= 65535, "View mip range");
    auto s = read_view(v);
    s.level = first;
    s.levels = count;
    s.flags |= 1;
    write_view(v, s);
}
API(nvnTextureViewSetLayers, void, NVNtextureView *v, int first, int count) {
    need(first >= 0 && first <= 65535 && count >= 0 && count <= 65535, "View layer range");
    auto s = read_view(v);
    s.layer = first;
    s.layers = count;
    s.flags |= 2;
    write_view(v, s);
}
API(nvnTextureViewSetFormat, void, NVNtextureView *v, NVNformat f) {
    need(int(f) >= 0 && int(f) <= 65535, "View format");
    auto s = read_view(v);
    s.format = f;
    s.flags |= 4;
    write_view(v, s);
}
API(nvnTextureViewSetTarget, void, NVNtextureView *v, NVNtextureTarget t) {
    need(int(t) >= 0 && int(t) <= 255, "View target");
    auto s = read_view(v);
    s.target = t;
    s.flags |= 8;
    write_view(v, s);
}
API(nvnTextureViewGetLevels, NVNboolean, const NVNtextureView *v, int *first, int *count) {
    auto s = read_view(v);
    if (!(s.flags & 1))
        return 0;
    *first = s.level;
    *count = s.levels;
    return 1;
}
API(nvnTextureViewGetLayers, NVNboolean, const NVNtextureView *v, int *first, int *count) {
    auto s = read_view(v);
    if (!(s.flags & 2))
        return 0;
    *first = s.layer;
    *count = s.layers;
    return 1;
}
API(nvnTextureViewGetFormat, NVNboolean, const NVNtextureView *v, NVNformat *f) {
    auto s = read_view(v);
    if (!(s.flags & 4))
        return 0;
    *f = NVNformat(s.format);
    return 1;
}
API(nvnTextureViewGetTarget, NVNboolean, const NVNtextureView *v, NVNtextureTarget *t) {
    auto s = read_view(v);
    if (!(s.flags & 8))
        return 0;
    *t = NVNtextureTarget(s.target);
    return 1;
}
// NVN 53 control-memory positions retain the latest recording at that position.
// This lifetime rule follows MacNativeGames' BOTW frontend; handles are resolved
// and inlined before callbacks can recycle source command storage.
struct Recorder {
    uint8_t *control{};size_t control_size{},control_used{},command_size{},command_used{};
    size_t recording_guest_start{};
    std::vector<std::pair<uintptr_t,size_t>> segments;
    std::vector<uint8_t> arena;
    TextureRefs textures;
};
std::map<const cmd_t*,Recorder> recorders;
std::map<uintptr_t,size_t> command_ranges;
struct CommandCopy {
    std::vector<uint8_t> bytes;
    TextureRefs textures;
    ~CommandCopy(){std::lock_guard lock(command_mutex);command_ranges.erase(uintptr_t(bytes.data()));}
};
std::map<uint64_t,std::shared_ptr<CommandCopy>> command_copies;
std::map<uint64_t,size_t> command_guest_bytes;
void retain_recorded_texture(NVNcommandBuffer *c, const std::shared_ptr<const tex_t> &texture) {
    recorders[&as<cmd_t>(c)].textures.emplace(texture->serial,texture);
}
size_t command_copy_bytes;
size_t recording_available(void *user,const void *address) {
    auto &r=*static_cast<Recorder*>(user);auto at=uintptr_t(address);
    for(auto [base,size]:r.segments)if(at>=base && at-base<size)return size-(at-base);
    return 0;
}
const std::vector<uint8_t> &command_copy(uint64_t handle) {
    auto it=command_copies.find(handle);need(it!=command_copies.end(),"Unknown command handle");return it->second->bytes;
}
uint8_t *allocate(cmd_t &c, size_t bytes) {
    need(c.recording && bytes <= 64u*1024*1024,"Invalid command recording");
    bytes=(bytes+7)&~size_t(7);auto &r=recorders[&c];
    size_t used=c.cur?size_t(c.cur-r.arena.data()):0;
    need(used+bytes<=64u*1024*1024,"Recording arena limit");
    if(used+bytes>r.arena.size()) {
        size_t capacity=std::max<size_t>(65536,r.arena.size());
        while(capacity<used+bytes)capacity*=2;
        r.arena.resize(capacity);c.begin=r.arena.data();
        r.segments={{uintptr_t(r.arena.data()),capacity}};
    }
    c.cur=r.arena.data()+used+bytes;c.end=r.arena.data()+r.arena.size();
    r.command_used+=bytes;auto *out=r.arena.data()+used;memset(out,0,bytes);return out;
}
// Guest resource heaps use these usage queries when budgeting persistent
// commands. Our fixed-capacity host payloads are not NVN pushbuffer sizes.
// Port the measured binding/draw accounting from MacNativeGames' MIT frontend
// (games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c, a02228e).
size_t guest_record_bytes(uint32_t op,const void *data,size_t fallback) {
    switch(op) {
    case OP_BIND_VATTR:return 68;
    case OP_BIND_VSTREAM:return 4*(16+static_cast<const p_bind_vstream*>(data)->n);
    case OP_BIND_VBUF:return 24*static_cast<const p_bind_vbuf*>(data)->n;
    case OP_BIND_UBO:case OP_BIND_SSBO:return 20;
    case OP_BIND_TEX:return 16+4*static_cast<const p_bind_handles*>(data)->n;
    case OP_BIND_IMG:case OP_BIND_SAMP:return 16+8*static_cast<const p_bind_handles*>(data)->n;
    case OP_DRAW_IDX:return 44;
    default:return fallback;
    }
}
template <class T> void record(NVNcommandBuffer *c, uint32_t op, const T &value,
                               size_t guest_bytes=SIZE_MAX) {
    auto bytes = (8 + sizeof(T) + 7) & ~size_t(7);
    auto *out = allocate(as<cmd_t>(c), bytes);
    op_hdr h{op, uint32_t(bytes)};
    memcpy(out, &h, 8);
    memcpy(out + 8, &value, sizeof(T));
    auto &r=recorders[&as<cmd_t>(c)];
    r.command_used-=bytes;
    r.command_used+=guest_bytes==SIZE_MAX?guest_record_bytes(op,&value,bytes):guest_bytes;
}
API(nvnDeviceBuilderSetDefaults, void, NVNdeviceBuilder *b) { as<dev_builder_t>(b) = {}; }
API(nvnDeviceBuilderSetFlags, void, NVNdeviceBuilder *b, int flags) {
    as<dev_builder_t>(b).flags = flags;
}
API(nvnDeviceInitialize, NVNboolean, NVNdevice *d, const NVNdeviceBuilder *b) {
    LOCK;
    need(ready, "Vulkan is not initialized");
    as<dev_t_>(d) = {MAGIC('D', 'E', 'V', 0), as<dev_builder_t>(b).flags};
    return 1;
}
API(nvnDeviceFinalize, void, NVNdevice *d) {
    LOCK;
    xbg_legacy_finish();
    as<dev_t_>(d).magic = 0;
}
API(nvnDeviceGetProcAddress, PFNNVNGENERICFUNCPTRPROC, const NVNdevice *, const char *name) {
    auto p=mng_nvn_get_proc_address(name);
    if(!p && getenv("MNG_NVN_LOOKUP_LOG"))fprintf(stderr,"[nvn-missing] %s\n",name);
    return reinterpret_cast<PFNNVNGENERICFUNCPTRPROC>(host_wrapper?host_wrapper(p,name):p);
}
API(nvnDeviceGetTimestampInNanoseconds, uint64_t, const NVNdevice *, const NVNcounterData *data) {
    need(data,"Null counter data");return uint64_t((__uint128_t(data->timestamp)*625)/12);
}
API(nvnDeviceGetInteger, void, const NVNdevice *, NVNdeviceInfo property, int *out) {
    // Frontend ABI constants do not depend on mutable renderer state.
    need(out,"Null device property output");
    switch(property) {
        case NVN_DEVICE_INFO_API_MAJOR_VERSION: *out=53;return;
        case NVN_DEVICE_INFO_API_MINOR_VERSION: *out=311;return;
        // Native frontend storage and descriptor ABI; GPU limits are queried below.
        case NVN_DEVICE_INFO_UNIFORM_BUFFER_BINDINGS_PER_STAGE: *out=14;return;
        case NVN_DEVICE_INFO_UNIFORM_BUFFER_ALIGNMENT: *out=256;return;
        case NVN_DEVICE_INFO_SHADER_STORAGE_BUFFER_BINDINGS_PER_STAGE: *out=16;return;
        case NVN_DEVICE_INFO_TEXTURE_BINDINGS_PER_STAGE: *out=32;return;
        case NVN_DEVICE_INFO_IMAGE_BINDINGS_PER_STAGE: *out=8;return;
        case NVN_DEVICE_INFO_COUNTER_ALIGNMENT: *out=16;return;
        case NVN_DEVICE_INFO_ZCULL_SAVE_RESTORE_ALIGNMENT: *out=32;return;
        case NVN_DEVICE_INFO_INDIRECT_DRAW_ALIGNMENT: *out=4;return;
        case NVN_DEVICE_INFO_TEXTURE_DESCRIPTOR_SIZE: *out=32;return;
        case NVN_DEVICE_INFO_SAMPLER_DESCRIPTOR_SIZE: *out=32;return;
        case NVN_DEVICE_INFO_RESERVED_TEXTURE_DESCRIPTORS: *out=256;return;
        case NVN_DEVICE_INFO_RESERVED_SAMPLER_DESCRIPTORS: *out=256;return;
        case NVN_DEVICE_INFO_COMMAND_BUFFER_COMMAND_ALIGNMENT: *out=4;return;
        case NVN_DEVICE_INFO_COMMAND_BUFFER_CONTROL_ALIGNMENT: *out=8;return;
        case NVN_DEVICE_INFO_COMMAND_BUFFER_MIN_COMMAND_SIZE: *out=512;return;
        case NVN_DEVICE_INFO_COMMAND_BUFFER_MIN_CONTROL_SIZE: *out=256;return;
        case NVN_DEVICE_INFO_SHADER_SCRATCH_MEMORY_SCALE_FACTOR_MINIMUM: *out=2;return;
        case NVN_DEVICE_INFO_SHADER_SCRATCH_MEMORY_SCALE_FACTOR_RECOMMENDED: *out=256;return;
        case NVN_DEVICE_INFO_SHADER_SCRATCH_MEMORY_ALIGNMENT: *out=4096;return;
        case NVN_DEVICE_INFO_SHADER_SCRATCH_MEMORY_GRANULARITY: *out=131072;return;
        case NVN_DEVICE_INFO_SHADER_SCRATCH_MEMORY_COMPUTE_SCALE_FACTOR_MINIMUM: *out=128;return;
        case NVN_DEVICE_INFO_MAX_TEXTURE_POOL_SIZE: *out=1048576;return;
        case NVN_DEVICE_INFO_MAX_SAMPLER_POOL_SIZE: *out=4096;return;
        case NVN_DEVICE_INFO_MEMPOOL_TEXTURE_OBJECT_PAGE_ALIGNMENT: *out=4096;return;
        case NVN_DEVICE_INFO_MAX_TEXTURE_LEVELS: *out=15;return;
        case NVN_DEVICE_INFO_GLSLC_MAX_SUPPORTED_GPU_CODE_MAJOR_VERSION: *out=1;return;
        case NVN_DEVICE_INFO_GLSLC_MIN_SUPPORTED_GPU_CODE_MAJOR_VERSION: *out=1;return;
        case NVN_DEVICE_INFO_GLSLC_MAX_SUPPORTED_GPU_CODE_MINOR_VERSION: *out=14;return;
        case NVN_DEVICE_INFO_GLSLC_MIN_SUPPORTED_GPU_CODE_MINOR_VERSION: *out=5;return;
        case NVN_DEVICE_INFO_INDIRECT_DISPATCH_ALIGNMENT: *out=4;return;
        case NVN_DEVICE_INFO_LINEAR_TEXTURE_STRIDE_ALIGNMENT: *out=32;return;
        case NVN_DEVICE_INFO_LINEAR_RENDER_TARGET_STRIDE_ALIGNMENT: *out=128;return;
        case NVN_DEVICE_INFO_MEMORY_POOL_PAGE_SIZE: *out=65536;return;
        case NVN_DEVICE_INFO_UNIFORM_BUFFER_UPDATE_ALIGNMENT: *out=4;return;
        case NVN_DEVICE_INFO_MAX_PRESENT_INTERVAL: *out=5;return;
        case NVN_DEVICE_INFO_SUPPORTS_IMAGES_USING_TEXTURE_HANDLES: *out=1;return;
        case NVN_DEVICE_INFO_SHADER_CODE_MEMORY_POOL_PADDING_SIZE: *out=1024;return;
        case NVN_DEVICE_INFO_QUEUE_COMMAND_MEMORY_GRANULARITY: *out=4096;return;
        case NVN_DEVICE_INFO_QUEUE_COMMAND_MEMORY_MIN_SIZE: *out=65536;return;
        case NVN_DEVICE_INFO_QUEUE_COMMAND_MEMORY_DEFAULT_SIZE: *out=65536;return;
        case NVN_DEVICE_INFO_QUEUE_COMPUTE_MEMORY_GRANULARITY: *out=256;return;
        case NVN_DEVICE_INFO_QUEUE_COMPUTE_MEMORY_MIN_SIZE: *out=16384;return;
        case NVN_DEVICE_INFO_QUEUE_COMPUTE_MEMORY_DEFAULT_SIZE: *out=262144;return;
        case NVN_DEVICE_INFO_QUEUE_COMMAND_MEMORY_MIN_FLUSH_THRESHOLD: *out=4096;return;
        case NVN_DEVICE_INFO_MAX_TEXTURES_PER_WINDOW: *out=4;return;
        case NVN_DEVICE_INFO_MIN_TEXTURES_PER_WINDOW: *out=2;return;
        case NVN_DEVICE_INFO_SUPPORTS_DEBUG_LAYER: *out=0;return;
        case NVN_DEVICE_INFO_QUEUE_CONTROL_MEMORY_MIN_SIZE: *out=4096;return;
        case NVN_DEVICE_INFO_QUEUE_CONTROL_MEMORY_DEFAULT_SIZE: *out=16384;return;
        case NVN_DEVICE_INFO_QUEUE_CONTROL_MEMORY_GRANULARITY: *out=4096;return;
        default: break;
    }
    HostWait host_wait;
    LOCK;
    if(!xbg_device_integer(property,out))xbg_fatalf("Unsupported NVN device property %d",int(property));
}
API(nvnDeviceSetWindowOriginMode, void, NVNdevice *d, NVNwindowOriginMode mode) {
    need(mode == NVN_WINDOW_ORIGIN_MODE_UPPER_LEFT, "Only upper-left window origin is implemented");
    as<dev_t_>(d).origin = mode;
}
API(nvnDeviceGetWindowOriginMode, NVNwindowOriginMode, const NVNdevice *d) {
    return NVNwindowOriginMode(as<dev_t_>(d).origin);
}
API(nvnDeviceSetDepthMode, void, NVNdevice *d, NVNdepthMode mode) {
    need(mode == NVN_DEPTH_MODE_NEAR_IS_MINUS_W, "Shader bundle requires near-minus-w depth");
    as<dev_t_>(d).depth_mode = mode;
}
API(nvnDeviceGetDepthMode, NVNdepthMode, const NVNdevice *d) {
    return NVNdepthMode(as<dev_t_>(d).depth_mode);
}
API(nvnDeviceGetTextureHandle, NVNtextureHandle, const NVNdevice *, int texture, int sampler) {
    need(texture >= 0 && texture < (1<<20) && sampler >= 0 && sampler < (1<<12), "Descriptor handle range");
    return uint64_t(texture) | (uint64_t(sampler) << 20);
}
API(nvnDeviceGetTexelFetchHandle, NVNtextureHandle, const NVNdevice *d, int texture) {
    return nvnDeviceGetTextureHandle(d, texture, 0);
}
API(nvnDeviceGetImageHandle, NVNimageHandle, const NVNdevice *d, int texture) {
    return nvnDeviceGetTextureHandle(d, texture, 0);
}
API(nvnDeviceInstallDebugCallback, void, NVNdevice *d, PFNNVNDEBUGCALLBACKPROC cb, void *user, uint32_t enabled) {
    as<dev_t_>(d).dbg_cb = enabled ? reinterpret_cast<void *>(cb) : nullptr;
    as<dev_t_>(d).dbg_data = user;
}
// Vulkan selects fast-clear encodings; NVN registration is unavailable.
API(nvnDeviceRegisterFastClearColor, NVNboolean, NVNdevice *, const float *, NVNformat) { return 0; }
API(nvnDeviceRegisterFastClearColori, NVNboolean, NVNdevice *, const int *, NVNformat) { return 0; }
API(nvnDeviceRegisterFastClearColorui, NVNboolean, NVNdevice *, const uint32_t *, NVNformat) { return 0; }
API(nvnDeviceRegisterFastClearDepth, NVNboolean, NVNdevice *, float) { return 0; }
API(nvnDeviceFinalizeCommandHandle, void, NVNdevice *, NVNcommandHandle handle) {
    COMMAND_LOCK;
    auto it=command_copies.find(handle);need(it!=command_copies.end(),"Finalize unknown command handle");
    // Active synchronous submissions pin the immutable copy independently.
    command_copy_bytes-=it->second->bytes.size();command_copies.erase(it);
    command_guest_bytes.erase(handle);
}

API(nvnMemoryPoolBuilderSetDefaults, void, NVNmemoryPoolBuilder *b) {
    auto *d = as<pool_builder_t>(b).dev;
    as<pool_builder_t>(b) = {};
    as<pool_builder_t>(b).dev = d;
}
API(nvnMemoryPoolBuilderSetDevice, void, NVNmemoryPoolBuilder *b, NVNdevice *d) {
    as<pool_builder_t>(b).dev = d;
}
API(nvnMemoryPoolBuilderSetStorage, void, NVNmemoryPoolBuilder *b, void *p, size_t n) {
    as<pool_builder_t>(b).mem = p;
    as<pool_builder_t>(b).size = n;
}
API(nvnMemoryPoolBuilderSetFlags, void, NVNmemoryPoolBuilder *b, int f) {
    as<pool_builder_t>(b).flags = f;
}
API(nvnMemoryPoolInitialize, NVNboolean, NVNmemoryPool *p, const NVNmemoryPoolBuilder *b) {
    LOCK;
    auto &s = as<pool_builder_t>(b);
    const bool physical=s.flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL;
    const bool virtual_pool=s.flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL;
    if (!s.size || (physical && virtual_pool) || (!virtual_pool && !s.mem)) return 0;
    if (s.size > UINTPTR_MAX - uintptr_t(s.mem)) return 0;
    auto &out=as<pool_t>(p);if(pool_backings.count(&out))return 0;
    PoolBacking backing;void *memory=s.mem;
    if (physical || virtual_pool) {
        if(s.size%65536 || (virtual_pool && memory) || (physical && uintptr_t(memory)%65536)) return 0;
        if(physical) {
            if(!memory_validator || !memory_validator(uintptr_t(memory),s.size))return 0;
            // MAP_FIXED replaces CPU backing too; retire reports before staging it.
            xbg_legacy_finish();
#if defined(__APPLE__)
            // Private anonymous backing, immediately unlinked; mmap aliases
            // retain the file description until the physical pool retires.
            char name[64];snprintf(name,sizeof name,"/mng-nvn-%d-%llu",getpid(),
                (unsigned long long)nvn_new_serial());
            const int fd=shm_open(name,O_RDWR|O_CREAT|O_EXCL,0600);
            if(fd>=0){shm_unlink(name);fcntl(fd,F_SETFD,FD_CLOEXEC);}
#else
            const int fd=int(syscall(__NR_memfd_create,"mng-nvn-pool",1));
#endif
            if(fd<0)return 0;
            if(ftruncate(fd,off_t(s.size))){close(fd);return 0;}
            void *staging=mmap(nullptr,s.size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
            if(staging==MAP_FAILED){close(fd);return 0;}
            memcpy(staging,memory,s.size);
            void *mapped=mmap(memory,s.size,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0);
            munmap(staging,s.size);
            if(mapped==MAP_FAILED){close(fd);return 0;}
            backing.fd=fd;
        } else {
            memory=mmap(nullptr,s.size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            if(memory==MAP_FAILED)return 0;
            backing.virtual_range=true;
        }
    }
    out={MAGIC('P','O','O','L'),s.flags,static_cast<uint8_t*>(memory),s.size};
    if(!physical && !virtual_pool)backing.report_pool=std::make_shared<mng::port::ReportPool>(uintptr_t(memory),s.size,nvn_new_serial());
    pool_backings[&out]=backing;
    xbg_legacy_pool_add(memory,s.size);
    return 1;
}
API(nvnMemoryPoolFinalize, void, NVNmemoryPool *p) {
    LOCK;
    auto &s = as<pool_t>(p);
    xbg_legacy_finish();
    xbg_legacy_pool_del(s.mem);
    if(auto it=pool_backings.find(&s);it!=pool_backings.end()) {
        if(it->second.report_pool)it->second.report_pool->alive.store(false,std::memory_order_release);
        if(it->second.fd>=0)close(it->second.fd);
        if(it->second.virtual_range)munmap(s.mem,s.size);
        pool_backings.erase(it);
    }
    s = {};
}
API(nvnMemoryPoolMapVirtual, NVNboolean, NVNmemoryPool *pool, int count, const NVNmappingRequest *requests) {
    LOCK;auto &v=as<pool_t>(pool);
    if(!(v.flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) || count<0 || count>1048576 || (count && !requests))return 0;
    // The dependency leaves this public request opaque. The native Zelda
    // adapter uses a 40-byte descriptor; runtime ranges are validated below.
    struct MappingRequest {const NVNmemoryPool *physicalPool;int64_t physicalOffset,virtualOffset;uint64_t size;int32_t storageClass;uint32_t padding;};
    static_assert(sizeof(MappingRequest)==40);
    const auto *mapping=reinterpret_cast<const MappingRequest*>(requests);
    for(int i=0;i<count;++i) {
        const auto &r=mapping[i];
        if(!r.size || (uint64_t(r.virtualOffset)|uint64_t(r.physicalOffset)|r.size)%65536 || !range(v.size,r.virtualOffset,r.size))return 0;
        if(r.physicalPool) {
            const auto &p=as<pool_t>(r.physicalPool);auto b=pool_backings.find(&p);
            if(!(p.flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL) || !range(p.size,r.physicalOffset,r.size) || b==pool_backings.end() || b->second.fd<0)return 0;
        }
    }
    xbg_legacy_finish();
    for(int i=0;i<count;++i) {
        const auto &r=mapping[i];int fd=-1;int flags=MAP_PRIVATE|MAP_ANONYMOUS;off_t offset=0;
        if(r.physicalPool){fd=pool_backings.at(&as<pool_t>(r.physicalPool)).fd;flags=MAP_SHARED;offset=r.physicalOffset;}
        need(mmap(v.mem+r.virtualOffset,r.size,PROT_READ|PROT_WRITE,flags|MAP_FIXED,fd,offset)!=MAP_FAILED,"Virtual pool mapping failed");
    }
    invalidate_pool_textures(&v);
    return 1;
}
API(nvnMemoryPoolMap, void *, const NVNmemoryPool *p) { return as<pool_t>(p).mem; }
API(nvnMemoryPoolGetBufferAddress, NVNbufferAddress, const NVNmemoryPool *p) {
    return uintptr_t(as<pool_t>(p).mem);
}
API(nvnMemoryPoolGetSize, size_t, const NVNmemoryPool *p) { return as<pool_t>(p).size; }
API(nvnMemoryPoolGetFlags, NVNmemoryPoolFlags, const NVNmemoryPool *p) {
    return NVNmemoryPoolFlags(as<pool_t>(p).flags);
}
// A buffer is an address/size descriptor, not another allocation. MacNative's
// BOTW path accepts a 256 KiB descriptor backed by a 144 KiB pool. Cache
// maintenance only affects bytes backed by that pool; it must not dereference
// or claim ownership of the descriptor's unbacked suffix.
size_t pool_cache_bytes(const pool_t& pool,ptrdiff_t offset,size_t requested){
    need(offset>=0 && uint64_t(offset)<=pool.size,"Pool cache offset outside backing");
    const size_t backed=std::min<uint64_t>(requested,pool.size-uint64_t(offset));
    if(backed!=requested){static std::atomic<unsigned> notes{};
        if(notes.fetch_add(1,std::memory_order_relaxed)<8)
            xbg_logf("NVN pool cache range clipped to backing: pool=%llu offset=%lld requested=%llu backed=%llu",
                (unsigned long long)pool.size,(long long)offset,(unsigned long long)requested,(unsigned long long)backed);
    }
    return backed;
}
API(nvnMemoryPoolFlushMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t bytes) {
    const auto &pool=as<pool_t>(p);
    bytes=pool_cache_bytes(pool,off,bytes);
    if(!bytes)return;
    static const bool range_flush=[] {const char *v=getenv("MNG_RANGE_FLUSH");return !v || strcmp(v,"0");}();
    // Ordinary CPU uploads are coherent host memory. Flush does not order a
    // different thread's future submissions. CPU-output dependencies retain
    // the old path; physical/virtual pools may alias another GPU VA.
    const bool aliases=pool.flags&(NVN_MEMORY_POOL_FLAGS_PHYSICAL|NVN_MEMORY_POOL_FLAGS_VIRTUAL);
    // A normal coherent upload only publishes CPU visibility. Query current
    // output ownership through its independent short lock, never front_mutex.
    // A future operation is not a dependency of this Flush. Unknown aliases and
    // queued translation retain the old ordering until their backing/input
    // ownership can be published before queue admission.
    if(range_flush && live_range_flush.load(std::memory_order_relaxed) && !aliases &&
       !ordered_flush_fallback.load(std::memory_order_relaxed)){
        const unsigned kinds=mng::port::flush_dependencies().match(uintptr_t(pool.mem)+off,bytes);
        spatial::LiteTrace::counter("NVN.Flush.LiveKinds",kinds);
        if(!kinds){
            ++flush_live_fast;spatial::LiteTraceScope scope("NVN.Flush.LiveFast");
            std::atomic_thread_fence(std::memory_order_release);return;
        }
        // A Vulkan Image owns separate device storage, not the Guest CPU bytes.
        // Persistent image authority is not a CPU-output dependency of Flush.
        // CPU-visible copies/queries/events retain their independent leases and
        // still take the old path. Invalidate, GPU fences and readback are unchanged.
        if(kinds==mng::port::LiveRanges::Image && image_flush.load(std::memory_order_acquire)){
            ++flush_image_fast;spatial::LiteTraceScope scope("NVN.Flush.ImagePublish");
            std::atomic_thread_fence(std::memory_order_release);return;
        }
        ++flush_live_conflict;if(kinds&mng::port::LiveRanges::Unknown)++flush_unknown;
        spatial::LiteTraceScope scope("NVN.Flush.LiveConflict");
        HostWait host_wait;LOCK;
        std::atomic_thread_fence(std::memory_order_release);return;
    }
    ++flush_legacy;
    const auto match=mapped_gpu_ranges.match(uintptr_t(pool.mem)+off,bytes);
    note_flush(uintptr_t(pool.mem)+off,bytes);
    spatial::LiteTrace::counter("NVN.Flush.RangeMatch",int(match));
    if(range_flush && !aliases && match==mng::port::ConservativeRanges::Match::Clear) {
        spatial::LiteTraceScope scope("NVN.Flush.Fast");
        std::atomic_thread_fence(std::memory_order_release);
        return;
    }
    spatial::LiteTraceScope scope("NVN.Flush.Conservative");
    spatial::LiteTrace::counter("NVN.Flush.PoolFlags",pool.flags);
    HostWait host_wait;
    LOCK;
    std::atomic_thread_fence(std::memory_order_release);
}
API(nvnMemoryPoolInvalidateMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t bytes) {
    const auto pool=as<pool_t>(p);
    bytes=pool_cache_bytes(pool,off,bytes);
    if(!bytes)return;
    const bool aliases=pool.flags&(NVN_MEMORY_POOL_FLAGS_PHYSICAL|NVN_MEMORY_POOL_FLAGS_VIRTUAL);
    if(invalidate_fast.load(std::memory_order_acquire) && range_invalidate_enabled.load(std::memory_order_acquire) &&
       !aliases && !ordered_flush_fallback.load(std::memory_order_acquire)){
        const unsigned kinds=mng::port::flush_dependencies().match(uintptr_t(pool.mem)+off,bytes);
        spatial::LiteTrace::counter("NVN.Invalidate.LiveKinds",kinds);
        // Image authority refers to separate device storage. CPU-visible
        // outputs have independent leases; unknown (including future) kinds
        // fail closed. The registry mutex never nests front/engine locks.
        if(!(kinds & ~unsigned(mng::port::LiveRanges::Image))){
            ++invalidate_clear;spatial::LiteTraceScope scope("NVN.Invalidate.Fast");
            std::atomic_thread_fence(std::memory_order_acquire);return;
        }
        ++invalidate_conflict;if(kinds&mng::port::LiveRanges::Unknown)++invalidate_unknown;
    }else ++invalidate_legacy;
    spatial::LiteTraceScope scope("NVN.Invalidate.Resolve");
    HostWait host_wait;
    LOCK;
    // CPU cache invalidation is a range operation, not QueueFinish. Uploaded
    // inputs are owned snapshots; resolve intersecting GPU-to-CPU results only.
    // Aliased pools retain the full path until physical ranges are tracked.
    if(pool.flags&(NVN_MEMORY_POOL_FLAGS_PHYSICAL|NVN_MEMORY_POOL_FLAGS_VIRTUAL))xbg_legacy_finish();
    else mng_nvn_invalidate_range(uintptr_t(pool.mem)+off,bytes);
    std::atomic_thread_fence(std::memory_order_acquire);
}
API(nvnBufferBuilderSetDefaults, void, NVNbufferBuilder *b) { as<Buffer>(b) = {}; }
API(nvnBufferBuilderSetDevice, void, NVNbufferBuilder *, NVNdevice *) {} // Device is process-owned.
API(nvnBufferBuilderSetStorage, void, NVNbufferBuilder *b, NVNmemoryPool *p, ptrdiff_t o,
    size_t n) {
    as<Buffer>(b) = {p, o, n};
}
API(nvnBufferInitialize, NVNboolean, NVNbuffer *b, const NVNbufferBuilder *builder) {
    auto s = as<Buffer>(builder);
    if (!s.pool || !range(as<pool_t>(s.pool).size,s.offset,0))
        return 0;
    const uintptr_t base=uintptr_t(as<pool_t>(s.pool).mem);
    if(uint64_t(s.offset)>UINTPTR_MAX-base || s.bytes>UINTPTR_MAX-(base+uint64_t(s.offset)))return 0;
    as<Buffer>(b) = s;
    return 1;
}
API(nvnBufferFinalize, void, NVNbuffer *b) { as<Buffer>(b) = {}; }
API(nvnBufferMap, void *, const NVNbuffer *b) {
    auto &s = as<Buffer>(b);
    return as<pool_t>(s.pool).mem + s.offset;
}
API(nvnBufferGetAddress, NVNbufferAddress, const NVNbuffer *b) {
    return uintptr_t(nvnBufferMap(b));
}
API(nvnBufferGetSize, size_t, const NVNbuffer *b) { return as<Buffer>(b).bytes; }
API(nvnBufferFlushMappedRange, void, const NVNbuffer *b, ptrdiff_t o, size_t n) {
    auto &s = as<Buffer>(b);
    need(range(s.bytes, o, n), "Buffer flush range");
    nvnMemoryPoolFlushMappedRange(s.pool, s.offset + o, n);
}
API(nvnBufferInvalidateMappedRange, void, const NVNbuffer *b, ptrdiff_t o, size_t n) {
    auto &s = as<Buffer>(b);
    need(range(s.bytes, o, n), "Buffer invalidate range");
    nvnMemoryPoolInvalidateMappedRange(s.pool, s.offset + o, n);
}

API(nvnTextureBuilderSetDefaults, void, NVNtextureBuilder *b) {
    // Builder state lives entirely in the caller's NVN object, as on Mac.
    // It has no renderer ownership and must not wait behind translation.
    auto *d = texture_builder(b).value.dev;
    auto &t = texture_builder(b).value;
    t = {};
    t.dev = d;
    texture_builder(b).swizzle={2,3,4,5};
    t.d.target = NVN_TEXTURE_TARGET_2D;
    t.d.width = t.d.height = t.d.depth = t.d.levels = 1;
}
API(nvnTextureBuilderSetDevice, void, NVNtextureBuilder *b, NVNdevice *d) {
    texture_builder(b).value.dev = d;
}
#define TEX_SET(Name, Field, Type)                                                                 \
    API(nvnTextureBuilderSet##Name, void, NVNtextureBuilder *b, Type v) {                          \
        texture_builder(b).value.d.Field = v;                                                          \
    }
TEX_SET(Flags, flags, int)
TEX_SET(Target, target, NVNtextureTarget)
TEX_SET(Format, format, NVNformat) TEX_SET(Width, width, int) TEX_SET(Height, height, int)
    TEX_SET(Depth, depth, int) TEX_SET(Levels, levels, int) TEX_SET(Samples, samples, int)
        TEX_SET(Stride, stride, ptrdiff_t)
            API(nvnTextureBuilderSetSize2D, void, NVNtextureBuilder *b, int w, int h) {
    auto &d = texture_builder(b).value.d;
    d.width = w;
    d.height = h;
    d.depth = 1;
}
API(nvnTextureBuilderSetSize3D, void, NVNtextureBuilder *b, int w, int h, int z) {
    auto &d = texture_builder(b).value.d;
    d.width = w;
    d.height = h;
    d.depth = z;
}
API(nvnTextureBuilderSetStorage, void, NVNtextureBuilder *b, NVNmemoryPool *p, ptrdiff_t o) {
    texture_builder(b).value.pool = &as<pool_t>(p);
    texture_builder(b).value.off = o;
}
API(nvnTextureBuilderSetSwizzle, void, NVNtextureBuilder *b, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle blue, NVNtextureSwizzle a) {
    need(unsigned(r)<=5 && unsigned(g)<=5 && unsigned(blue)<=5 && unsigned(a)<=5,"Texture swizzle range");
    texture_builder(b).swizzle={uint8_t(r),uint8_t(g),uint8_t(blue),uint8_t(a)};
}
API(nvnTextureBuilderSetDepthStencilMode, void, NVNtextureBuilder *, NVNtextureDepthStencilMode mode) {
    need(mode==NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH,"Stencil-plane sampling unsupported");
}
API(nvnTextureBuilderGetStorageClass, NVNstorageClass, const NVNtextureBuilder *b) {
    return texture_builder(b).value.d.target==NVN_TEXTURE_TARGET_BUFFER ? 0 : 1;
}
API(nvnTextureGetStorageClass, NVNstorageClass, const NVNtexture *t) {
    return read_texture(t).d.target==NVN_TEXTURE_TARGET_BUFFER ? 0 : 1;
}
API(nvnTextureBuilderSetPackagedTextureLayout, void, NVNtextureBuilder *b,
    const NVNpackagedTextureLayout *layout) {
    auto &t = texture_builder(b).value;
    t.has_layout = layout != nullptr;
    if (layout)
        memcpy(t.layout, layout, 8);
    t.d.pkg_block_h = layout ? t.layout[0] + 1 : 0;
}
API(nvnTextureBuilderGetStorageSize, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout l{};
    nvn_tex_layout(&texture_builder(b).value.d, &l);
    return l.size;
}
API(nvnTextureBuilderGetStorageAlignment, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout l{};
    nvn_tex_layout(&texture_builder(b).value.d, &l);
    return l.align;
}
void release_texture(tex_t value){
    if(translation_owner){translation_owner->submit([value]()mutable{
        xbg_legacy_tex_release(&value);return uint64_t(0);
    },sizeof(value));return;}
    xbg_legacy_tex_release(&value);
}
int initialize_texture(NVNtexture *t,const NVNtextureBuilder *b,bool publish) {
    auto &s = texture_builder(b).value;
    nvn_texlayout l{};
    nvn_tex_layout(&s.d, &l);
    // Like the Mac frontend, retain format-NONE metadata for descriptor slots.
    // No image is allocated until use. Actual GPU use remains an error rather
    // than substituting a dummy format or silently dropping a draw.
    const bool unformatted=s.d.format==NVN_FORMAT_NONE;
    if ((!unformatted && (!l.bpb || !l.size)) || !s.pool || !range(s.pool->size, s.off, l.size))
        {fprintf(stderr,"[nvn] TextureInitialize rejected size=%llu offset=%lld pool=%p poolSize=%llu format=%d\n",(unsigned long long)l.size,(long long)s.off,s.pool,(unsigned long long)(s.pool?s.pool->size:0),s.d.format);return 0;}
    if(unformatted) {
        static std::atomic<unsigned> reports{};
        if(reports.fetch_add(1,std::memory_order_relaxed)<8)fprintf(stderr,"[nvn] Unformatted descriptor texture: target=%d extent=%ux%ux%u levels=%u offset=%lld\n",s.d.target,s.d.width,s.d.height,s.d.depth,s.d.levels,(long long)s.off);
    }
    // BOTW copies opaque textures by value from temporary objects. Each new
    // initialization must publish a new immutable identity, even if this slot
    // still holds the cookie of an older copy.
    auto previous=read_texture(t);
    const bool retire=previous.magic==MAGIC('T','E','X',0) && previous.serial;
    if(retire && !publish)release_texture(previous);
    tex_t o{};
    o.magic = MAGIC('T', 'E', 'X', 0);
    o.d = s.d;
    o.L = l;
    o.pool = s.pool;
    o.off = s.off;
    o.mem = s.pool->mem + s.off;
    // Include all textures, even read-only ones. Mirrored-image reconciliation
    // remains conservative; do not infer safety from absence in the GPU cache.
    mapped_gpu_ranges.include(uintptr_t(o.mem),o.L.size);
    const auto generation=nvn_new_serial();
    need(generation<=(UINT64_MAX>>12),"Texture generation overflow");
    o.serial = (generation<<12) | pack_swizzle(texture_builder(b).swizzle);
    o.has_layout = s.has_layout;
    memcpy(o.layout, s.layout, 8);
    auto commit=[&]()noexcept{write_texture(t,o);texture_initializations.fetch_add(1,std::memory_order_relaxed);};
    if(publish){Publication operation=RetireTexture{previous};if(!publications.publish(retire?&operation:nullptr,commit))return -1;}
    else commit();
    return 1;
}
API(nvnTextureInitialize, NVNboolean, NVNtexture *t, const NVNtextureBuilder *b) {
    if(publications.enabled()){const int result=initialize_texture(t,b,true);if(result>=0)return result;}
    PUBLISH_LOCK;return initialize_texture(t,b,false);
}
API(nvnTextureGetStorageSize, int, const NVNtexture *t) {
    need(read_texture(t).L.size<=INT32_MAX,"Texture storage size overflow");return int(read_texture(t).L.size);
}
// Depth compression/cull metadata belongs to the Vulkan driver. There is no
// separately addressable Tegra ZCull cache to serialize in this backend.
// Matches MacNativeGames' totk_nvn_ext.c policy, independently corroborated by
// Citron's host texture implementation (implementation code is not copied).
API(nvnTextureBuilderGetZCullStorageSize, size_t, const NVNtextureBuilder *b) {
    (void)texture_builder(b).value;return 0;
}
API(nvnTextureGetZCullStorageSize, size_t, const NVNtexture *t) {
    (void)read_texture(t);return 0;
}
API(nvnCommandBufferSaveZCullData, void, NVNcommandBuffer *c, NVNbufferAddress, size_t) {
    (void)as<cmd_t>(c);
}
API(nvnCommandBufferRestoreZCullData, void, NVNcommandBuffer *c, NVNbufferAddress, size_t) {
    (void)as<cmd_t>(c);
}
API(nvnTextureGetTextureAddress, NVNtextureAddress, const NVNtexture *t) {return uintptr_t(read_texture(t).mem);}
API(nvnTextureGetMemoryOffset, ptrdiff_t, const NVNtexture *t) {return read_texture(t).off;}
API(nvnTextureGetViewOffset, ptrdiff_t, const NVNtexture *t, const NVNtextureView *v) {
    auto view=translate_view(v,t);auto texture=read_texture(t);
    auto level=view.set_levels?view.base_level:0;auto layer=view.set_layers?view.min_layer:0;
    need(level>=0 && unsigned(level)<texture.d.levels && layer>=0 && unsigned(layer)<texture.L.layers,"Texture view offset range");
    return nvn_tex_level_offset(&texture.d,level)+uint64_t(layer)*texture.L.layer_size;
}
API(nvnTextureWriteTexelsStrided, void, const NVNtexture *texture, const NVNtextureView *v,
    const NVNcopyRegion *r, const void *data, ptrdiff_t row_stride, ptrdiff_t image_stride) {
    LOCK;need(r && data,"Null texture upload");auto t=read_texture(texture);auto view=translate_view(v,texture);
    need(!view.set_format || view.format==t.d.format,"Upload view format reinterpretation unsupported");
    unsigned level=view.set_levels?view.base_level:0,layer=view.set_layers?view.min_layer:0;
    need(level<t.d.levels && layer<t.L.layers && r->xoffset>=0 && r->yoffset>=0 && r->zoffset>=0 && r->width>0 && r->height>0 && r->depth>0,"Texture upload range");
    int bw,bh;int bpb=nvn_fmt_bpb(t.d.format,&bw,&bh);need(bpb>0,"Texture upload format");
    need(r->xoffset%bw==0 && r->yoffset%bh==0,"Texture upload block alignment");
    const uint32_t width=std::max(1u,t.d.width>>level),height=std::max(1u,t.d.height>>level);
    need(uint64_t(r->xoffset)+r->width<=width && uint64_t(r->yoffset)+r->height<=height,"Texture upload bounds");
    const uint32_t bytes=((r->width+bw-1)/bw)*bpb,rows=(r->height+bh-1)/bh;
    if(!row_stride)row_stride=bytes;if(!image_stride)image_stride=row_stride*rows;
    need(row_stride>=bytes && image_stride>=row_stride*rows,"Texture upload pitch");
    xbg_legacy_finish();xbg_texture_invalidate(&t);
    if(mng_linear_texture(&t.d)) {
        need(!level && !layer && !r->zoffset && r->depth==1 && t.d.samples<=1 &&
             (t.d.target==0 || t.d.target==1 || t.d.target==7),"Linear upload subresource");
        const size_t pitch=mng_linear_stride(&t.d);
        need(pitch>=size_t((width+bw-1)/bw)*bpb,"Linear upload storage pitch");
        for(uint32_t y=0;y<rows;++y){
            const uint64_t offset=uint64_t(r->yoffset/bh+y)*pitch+uint64_t(r->xoffset/bw)*bpb;
            need(offset<=t.L.size && bytes<=t.L.size-offset,"Linear upload storage range");
            memcpy(t.mem+offset,static_cast<const uint8_t*>(data)+uint64_t(y)*row_stride,bytes);
        }
        return;
    }
    for(int z=0;z<r->depth;++z) {
        const bool volume=t.d.target==NVN_TEXTURE_TARGET_3D;
        uint32_t row_bytes,total_rows,slices,block;uint64_t base;unsigned tile_h,tile_d;
        auto target_layer=volume?layer:layer+r->zoffset+z;
        need(nvn_deswizzle_geom(&t.d,level,target_layer,&row_bytes,&total_rows,&slices,&base,&tile_h,&tile_d,&block)==0,"Texture upload layout");
        uint32_t slice=volume?r->zoffset+z:0;need(slice<slices,"Texture upload depth");
        auto *src=static_cast<const uint8_t*>(data)+uint64_t(z)*image_stride;
        for(uint32_t y=0;y<rows;++y)for(uint32_t x=0;x<bytes;++x) {
            auto offset=base+nvn_bl_offset(uint32_t(r->xoffset/bw)*bpb+x,uint32_t(r->yoffset/bh)+y,slice,row_bytes,total_rows,tile_h,tile_d);
            need(offset<t.L.size,"Texture upload storage range");t.mem[offset]=src[uint64_t(y)*row_stride+x];
        }
    }
}
API(nvnTextureWriteTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const void *data) {
    nvnTextureWriteTexelsStrided(t,v,r,data,0,0);
}
API(nvnTextureFlushTexels, void, const NVNtexture *t, const NVNtextureView *, const NVNcopyRegion *) {
    LOCK;std::atomic_thread_fence(std::memory_order_release);auto value=read_texture(t);xbg_texture_invalidate(&value);
}
API(nvnTextureCompare, NVNboolean, const NVNtexture *a, const NVNtexture *b) {
    // Read-only inline values, matching the Metal frontend; no engine access.
    if(!publications.enabled()){
        LOCK;
        return mng::port::texture_equal(a,b,read_texture,swizzle_mask);
    }
    return mng::port::texture_equal(a,b,read_texture,swizzle_mask);
}
API(nvnTextureFinalize, void, NVNtexture *t) {
    spatial::LiteTraceScope scope("NVN.TextureFinalize");
    if(publications.enabled()){
        Publication operation=RetireTexture{read_texture(t)};
        if(publications.publish(&operation,[&]()noexcept{write_texture(t,{});texture_finalizations.fetch_add(1,std::memory_order_relaxed);}))return;
    }
    PUBLISH_LOCK;
    // Match MacNative's identity retirement: image_storage owns GPU contents,
    // submitted slots/present tickets retain their references, and pool release
    // or storage sweeping retires allocations behind a fence. A texture handle
    // disappearing is not a request to drain unrelated work on the queue.
    auto value=read_texture(t);release_texture(value);
    // Finalizing one by-value copy must not invalidate other guest copies.
    // Recorded commands and descriptors retain their immutable snapshots.
    write_texture(t,{});texture_finalizations.fetch_add(1,std::memory_order_relaxed);
}
#define TEX_GET(Name, Field, Type)                                                                 \
    API(nvnTextureGet##Name, Type, const NVNtexture *t) { return Type(read_texture(t).d.Field); }
TEX_GET(Flags, flags, NVNtextureFlags)
TEX_GET(Samples, samples, int)
TEX_GET(Stride, stride, ptrdiff_t)
TEX_GET(Width, width, int)
TEX_GET(Height, height, int)
TEX_GET(Depth, depth, int) TEX_GET(Levels, levels, int) TEX_GET(Format, format, NVNformat)
    TEX_GET(Target, target, NVNtextureTarget)
        API(nvnTextureReadTexels, void, const NVNtexture *t, const NVNtextureView *v,
            const NVNcopyRegion *r, void *out) {
    LOCK;
    auto view = translate_view(v,t);
    need(!(view.set_format || view.set_target),
         "Readback reinterpretation requires a staging view");
    need(r && out, "Null readback");
    auto value=read_texture(t);
    xbg_legacy_read_texels(&value, view.set_levels ? view.base_level : 0,
                           view.set_layers ? view.min_layer : 0, r, static_cast<uint8_t *>(out), 0,
                           0);
}

API(nvnCommandBufferInitialize, NVNboolean, NVNcommandBuffer *c, NVNdevice *d) {
    COMMAND_LOCK;
    as<cmd_t>(c) = {};
    as<cmd_t>(c).magic = MAGIC('C', 'M', 'D', 0);
    as<cmd_t>(c).dev = d;
    return 1;
}
API(nvnCommandBufferFinalize, void, NVNcommandBuffer *c) {
    COMMAND_LOCK;
    need(!as<cmd_t>(c).recording, "Finalize while recording");
    recorders.erase(&as<cmd_t>(c));
    // EndRecording owns its bytes and texture references independently. The
    // recorder may be destroyed while those handles remain executable (as in
    // MacNative's command probe); no renderer pointer refers to this Box.
    {std::lock_guard objects_lock(object_mutex);uint64_t id;memcpy(&id,c,sizeof id);objects.erase(id);memset(c,0,sizeof id);}
}
API(nvnCommandBufferSetMemoryCallback, void, NVNcommandBuffer *c,
    PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb) {
    as<cmd_t>(c).cb = cb;
}
API(nvnCommandBufferSetMemoryCallbackData, void, NVNcommandBuffer *c, void *data) {
    as<cmd_t>(c).cb_data = data;
}
API(nvnCommandBufferAddCommandMemory, void, NVNcommandBuffer *c, const NVNmemoryPool *p,
    ptrdiff_t o, size_t n) {
    COMMAND_LOCK;
    // BOTW also supplies a null pool with a nonzero declared capacity.
    // Like the Mac frontend, we only meter this capacity: host records never
    // access the guest range. A final capacity may extend past a pool's end.
    need(o >= 0 && n <= UINT64_MAX - uint64_t(o), "Command capacity overflow");
    if (p) need(uint64_t(o) <= as<pool_t>(p).size, "Command memory start");
    // Host records have a different size from Maxwell commands and independent
    // command buffers may share the same game storage. Use a private arena, as
    // the native Mac frontend does, and keep guest allocation accounting separate.
    auto &r=recorders[&as<cmd_t>(c)];r.command_size=n;r.command_used=0;
}
API(nvnCommandBufferAddControlMemory, void, NVNcommandBuffer *c, void *memory, size_t bytes) {
    COMMAND_LOCK;need(memory && bytes>=64,"Invalid control memory");auto &r=recorders[&as<cmd_t>(c)];
    r.control=static_cast<uint8_t*>(memory);r.control_size=bytes;r.control_used=0;
}
API(nvnCommandBufferBeginRecording, void, NVNcommandBuffer *c) {
    COMMAND_LOCK;
    auto &s = as<cmd_t>(c);
    need(!s.recording, "Nested recording");
    s.recording = 1;s.cur=nullptr;s.begin=nullptr;
    recorders[&s].textures.clear();
    auto *out = allocate(s, 8);
    op_hdr h{OP_BEGIN, 8};
    memcpy(out, &h, 8);
    s.begin = out;
    recorders[&s].command_used-=8;
    recorders[&s].recording_guest_start=recorders[&s].command_used;
}
API(nvnCommandBufferEndRecording, NVNcommandHandle, NVNcommandBuffer *c) {
    COMMAND_LOCK;
    auto &s = as<cmd_t>(c);
    auto *out = allocate(s, 8);
    op_hdr h{OP_END, 8};
    memcpy(out, &h, 8);
    s.recording = 0;
    auto &r=recorders[&s];r.command_used-=8;
    uint64_t source=uintptr_t(s.begin);size_t body=0;
    need(!xbg_command_clone(1,&source,nullptr,64u<<20,recording_available,&r,&body),"Invalid recorded command chain");
    std::vector<uint8_t> copy(body+16);
    op_hdr begin{OP_BEGIN,8},end{OP_END,8};memcpy(copy.data(),&begin,8);memcpy(copy.data()+8+body,&end,8);
    need(!xbg_command_clone(1,&source,copy.data()+8,body,recording_available,&r,&body),"Command snapshot failed");
    uint64_t handle=r.control && r.control_used+64<=r.control_size?uintptr_t(r.control+r.control_used):uintptr_t(copy.data());
    r.control_used+=64;
    auto previous=command_copies.find(handle);
    if(previous!=command_copies.end()) {
        // Latest recording at this control address; an active submission
        // retains its own shared copy until translation completes.
        command_copy_bytes-=previous->second->bytes.size();command_copies.erase(previous);
    }
    need(command_copy_bytes+copy.size()<=512u*1024*1024 && command_copies.size()<1048576,"Command snapshot retention limit");
    command_copy_bytes+=copy.size();
    auto retained=std::make_shared<CommandCopy>();retained->bytes=std::move(copy);retained->textures=std::move(r.textures);
    command_ranges[uintptr_t(retained->bytes.data())]=retained->bytes.size();
    command_copies.emplace(handle,std::move(retained));
    command_guest_bytes[handle]=r.command_used-r.recording_guest_start;
    return handle;
}
API(nvnCommandBufferIsRecording, NVNboolean, const NVNcommandBuffer *c) {
    return as<cmd_t>(c).recording != 0;
}
void inline_commands(NVNcommandBuffer *c,int n,const NVNcommandHandle *handles) {
    need(n>=0 && n<=1000000 && (!n || handles),"Command copy arguments");
    // EndRecording seals and validates these private immutable streams. Pin all
    // inputs before arena growth, which can recycle handles through a callback.
    std::vector<std::shared_ptr<CommandCopy>> sources; sources.reserve(n);
    size_t bytes=0,guest_bytes=0;
    auto &command=as<cmd_t>(c);auto &destination=recorders[&command];
    for(int i=0;i<n;++i){
        command_copy(handles[i]); // Validate the opaque handle before lookup.
        auto copy=command_copies.at(handles[i]);
        need(copy->bytes.size()>=16 && copy->bytes.size()-16<=(64u<<20)-bytes,"Command inline limit");
        bytes+=copy->bytes.size()-16;guest_bytes+=command_guest_bytes.at(handles[i]);
        destination.textures.insert(copy->textures.begin(),copy->textures.end());
        sources.push_back(std::move(copy));
    }
    static const bool bulk=[] {const char *v=getenv("MNG_BULK_COPY_COMMANDS");return !v || strcmp(v,"0");}();
    if(bulk && bytes){
        auto *out=allocate(command,bytes);
        for(const auto &copy:sources){const auto size=copy->bytes.size()-16;
            memcpy(out,copy->bytes.data()+8,size);out+=size;}
    }else if(!bulk){
        for(const auto &copy:sources)for(size_t at=8;at<copy->bytes.size()-8;){
            op_hdr h;memcpy(&h,copy->bytes.data()+at,sizeof h);
            need(h.size>=8 && !(h.size&7) && h.size<=copy->bytes.size()-8-at,"Invalid copied record");
            memcpy(allocate(command,h.size),copy->bytes.data()+at,h.size);at+=h.size;
        }
    }
    destination.command_used-=bytes;destination.command_used+=guest_bytes;
}
API(nvnCommandBufferCopyCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *h) {COMMAND_LOCK;inline_commands(c,n,h);}
API(nvnCommandBufferCallCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *h) {COMMAND_LOCK;inline_commands(c,n,h);}
API(nvnCommandBufferGetCommandMemorySize, size_t, const NVNcommandBuffer *c) {COMMAND_LOCK;return recorders[&as<cmd_t>(c)].command_size;}
API(nvnCommandBufferGetCommandMemoryUsed, size_t, const NVNcommandBuffer *c) {COMMAND_LOCK;auto &r=recorders[&as<cmd_t>(c)];return std::min(r.command_used,r.command_size);}
API(nvnCommandBufferGetCommandMemoryFree, size_t, const NVNcommandBuffer *c) {return nvnCommandBufferGetCommandMemorySize(c)-nvnCommandBufferGetCommandMemoryUsed(c);}
API(nvnCommandBufferGetControlMemorySize, size_t, const NVNcommandBuffer *c) {COMMAND_LOCK;return recorders[&as<cmd_t>(c)].control_size;}
API(nvnCommandBufferGetControlMemoryUsed, size_t, const NVNcommandBuffer *c) {COMMAND_LOCK;auto &r=recorders[&as<cmd_t>(c)];return std::min(r.control_used,r.control_size);}
API(nvnCommandBufferGetControlMemoryFree, size_t, const NVNcommandBuffer *c) {return nvnCommandBufferGetControlMemorySize(c)-nvnCommandBufferGetControlMemoryUsed(c);}
API(nvnCommandBufferGetMemoryCallback, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC, const NVNcommandBuffer *c) {return as<cmd_t>(c).cb;}
static void *nvnCommandBufferGetMemoryCallbackData(const NVNcommandBuffer *c) {return as<cmd_t>(c).cb_data;}
static Register register_CallbackData("nvnCommandBufferGetMemoryCallbackData",reinterpret_cast<void*>(nvnCommandBufferGetMemoryCallbackData));
API(nvnCommandBufferCopyBufferToBuffer, void, NVNcommandBuffer *c, NVNbufferAddress src,
    NVNbufferAddress dst, size_t size, int flags) {
    COMMAND_LOCK;
    need(!flags, "Unsupported buffer copy flags");
    const uint64_t args[]{src, dst, size};
    record(c, OP_COPY_B2B, args);
}
API(nvnCommandBufferCopyBufferToTexture, void, NVNcommandBuffer* c,NVNbufferAddress address,
    const NVNtexture* texture,const NVNtextureView* view,const NVNcopyRegion* region,int flags){
    COMMAND_LOCK;need(texture && region && address,"Buffer-to-texture copy arguments");
    p_copy_b2t p{};p.src=address;p.tex=snapshot(c,texture,"CopyBufferToTexture");
    p.view=translate_view(view,texture);p.has_view=view!=nullptr;p.r=*region;p.flags=flags;
    record(c,OP_COPY_B2T,p);
}
API(nvnCommandBufferCopyTextureToBuffer, void, NVNcommandBuffer* c,const NVNtexture* texture,
    const NVNtextureView* view,const NVNcopyRegion* region,NVNbufferAddress address,int flags){
    COMMAND_LOCK;need(texture && region && address,"Texture-to-buffer copy arguments");
    p_copy_b2t p{};p.src=address;p.tex=snapshot(c,texture,"CopyTextureToBuffer");
    p.view=translate_view(view,texture);p.has_view=view!=nullptr;p.r=*region;p.flags=flags;
    record(c,OP_COPY_T2B,p);
}
API(nvnCommandBufferSetCopyRowStride, void, NVNcommandBuffer* c,ptrdiff_t stride){
    COMMAND_LOCK;need(stride>=0,"Negative copy row stride");record(c,OP_COPY_STRIDE,p_copy_stride{stride});
}
API(nvnCommandBufferSetCopyImageStride, void, NVNcommandBuffer* c,ptrdiff_t stride){
    COMMAND_LOCK;need(stride>=0,"Negative copy image stride");record(c,OP_COPY_IMAGE_STRIDE,p_copy_stride{stride});
}
API(nvnCommandBufferUpdateUniformBuffer, void, NVNcommandBuffer *c, NVNbufferAddress buffer,
    size_t capacity, ptrdiff_t offset, size_t bytes, const void *data) {
    COMMAND_LOCK;
    need(range(capacity, offset, bytes) && data && bytes <= 65536, "Uniform update range");
    auto total = (24 + bytes + 7) & ~size_t(7);
    auto *out = allocate(as<cmd_t>(c), total);
    op_hdr h{OP_UPDATE_UB, uint32_t(total)};
    uint64_t args[]{buffer + offset, bytes};
    memcpy(out, &h, 8);
    memcpy(out + 8, args, 16);
    memcpy(out + 24, data, bytes);
}
API(nvnCommandBufferSetRenderTargets, void, NVNcommandBuffer *c, int n,
    const NVNtexture *const *color, const NVNtextureView *const *views, const NVNtexture *depth,
    const NVNtextureView *depth_view) {
    COMMAND_LOCK;
    need(n >= 0 && n <= 8 && (!n || color), "Render target count");
    p_set_rt p{};
    p.n = n;
    for (int i = 0; i < n; ++i) {
        if (color[i])
            p.color[i] = snapshot(c,color[i],"SetRenderTargets color");
        if (views && views[i]) {
            p.has_cview[i] = 1;
            p.cview[i] = translate_view(views[i],color[i],true);
        }
    }
    if (depth)
        p.depth = snapshot(c,depth,"SetRenderTargets depth");
    if (depth_view) {
        p.has_dview = 1;
        p.dview = translate_view(depth_view,depth,true);
    }
    record(c, OP_SET_RT, p);
}
API(nvnCommandBufferClearColor, void, NVNcommandBuffer *c, int index, const float *rgba, int mask) {
    COMMAND_LOCK;
    need(rgba && index >= 0 && index < 8 && !(mask & ~15), "Clear color arguments");
    p_clear_color p{index, mask, {}};
    memcpy(p.rgba, rgba, 16);
    record(c, OP_CLEAR_COLOR, p);
}
API(nvnCommandBufferClearDepthStencil, void, NVNcommandBuffer *c, float depth, uint32_t write,
    int stencil, int mask) {
    COMMAND_LOCK;
    record(c, OP_CLEAR_DS, p_clear_ds{depth, int(write & 255), stencil, mask});
}
API(nvnCommandBufferSetScissor, void, NVNcommandBuffer *c, int x, int y, int w, int h) {
    COMMAND_LOCK;
    record(c, OP_SCISSOR, p_scissor{x, y, w, h});
}
API(nvnCommandBufferSetDepthRange, void, NVNcommandBuffer *c, float n, float f) {
    COMMAND_LOCK;
    record(c, OP_DEPTH_RANGE, p_depth_range{n, f});
}
API(nvnCommandBufferSetViewport, void, NVNcommandBuffer *c, int x, int y, int w, int h) {
    COMMAND_LOCK;
    p_viewports p{};
    p.n = 1;
    p.v[0][0] = x;
    p.v[0][1] = y;
    p.v[0][2] = w;
    p.v[0][3] = h;
    p.v[0][5] = 1;
    record(c, OP_VIEWPORTS, p);
}
API(nvnCommandBufferReportCounter, void, NVNcommandBuffer *c, NVNcounterType type, NVNbufferAddress address) {
    COMMAND_LOCK;need(address && !(address & 15),"Counter address alignment");
    if(type!=NVN_COUNTER_TYPE_TIMESTAMP && type!=NVN_COUNTER_TYPE_TIMESTAMP_TOP && type!=NVN_COUNTER_TYPE_SAMPLES_PASSED)xbg_fatalf("Unsupported counter type %d",int(type));
    const uint64_t values[]={uint64_t(type),address};record(c,OP_REPORT_COUNTER,values);
}
API(nvnCommandBufferResetCounter, void, NVNcommandBuffer *c, NVNcounterType type) {
    COMMAND_LOCK;need(type==NVN_COUNTER_TYPE_SAMPLES_PASSED,"Unsupported counter reset");record(c,OP_RESET_COUNTER,uint64_t(type));
}
API(nvnCommandBufferSetShaderScratchMemory, void, NVNcommandBuffer *, const NVNmemoryPool *pool, ptrdiff_t offset, size_t bytes) {
    // SPIR-V private memory and register spills are allocated by the Vulkan
    // implementation. Validate the NVN reservation without exposing GPU VA.
    need(pool && range(as<pool_t>(pool).size,offset,bytes),"Shader scratch range");
}
API(nvnCommandBufferClearTexture, void, NVNcommandBuffer *c,const NVNtexture *t,const NVNtextureView *v,const NVNcopyRegion *r,const float *rgba,int mask) {
    COMMAND_LOCK;need(r && rgba,"Texture clear arguments");p_clear_tex p{};p.tex=snapshot(c,t,"ClearTexture");p.view=translate_view(v,t);p.has_view=v!=nullptr;p.r=*r;p.mask=mask;memcpy(p.c.f,rgba,16);record(c,OP_CLEAR_TEX,p);
}
API(nvnCommandBufferClearTextureui, void, NVNcommandBuffer *c,const NVNtexture *t,const NVNtextureView *v,const NVNcopyRegion *r,const uint32_t *rgba,int mask) {
    COMMAND_LOCK;need(r && rgba,"Texture clear arguments");p_clear_tex p{};p.tex=snapshot(c,t,"ClearTexture");p.view=translate_view(v,t);p.has_view=v!=nullptr;p.r=*r;p.mask=mask;p.is_uint=1;memcpy(p.c.u,rgba,16);record(c,OP_CLEAR_TEX,p);
}
API(nvnCommandBufferSetBlendColor, void, NVNcommandBuffer *c,const float *rgba) {COMMAND_LOCK;need(rgba,"Blend constant missing");p_blend_color p{};memcpy(p.rgba,rgba,16);record(c,OP_BLEND_COLOR,p);}
API(nvnCommandBufferSetStencilValueMask, void, NVNcommandBuffer *c,NVNface f,int value) {COMMAND_LOCK;record(c,OP_STENCIL,p_stencil{f,value,0,0,0});}
API(nvnCommandBufferSetStencilMask, void, NVNcommandBuffer *c,NVNface f,int value) {COMMAND_LOCK;record(c,OP_STENCIL,p_stencil{f,0,value,0,1});}
API(nvnCommandBufferSetStencilRef, void, NVNcommandBuffer *c,NVNface f,int value) {COMMAND_LOCK;record(c,OP_STENCIL,p_stencil{f,0,0,value,2});}
API(nvnCommandBufferSetPolygonOffsetClamp, void, NVNcommandBuffer *c,float factor,float units,float clamp) {COMMAND_LOCK;record(c,OP_POLY_OFFSET,p_poly_offset{factor,units,clamp});}
API(nvnCommandBufferSetDepthClamp, void, NVNcommandBuffer *c,uint32_t enabled) {COMMAND_LOCK;record(c,OP_COUNT_+10,uint32_t((enabled&255)!=0));}
API(nvnCommandBufferSetRasterizerDiscard, void, NVNcommandBuffer *c,uint32_t enabled) {COMMAND_LOCK;record(c,OP_COUNT_+12,uint32_t((enabled&255)!=0));}
API(nvnCommandBufferSetLineWidth, void, NVNcommandBuffer *c,float width) {
    COMMAND_LOCK;need(std::isfinite(width) && width>0,"Invalid line width");record(c,OP_COUNT_+11,width);
}
API(nvnCommandBufferSetPointSize, void, NVNcommandBuffer *c,float size) {
    // The offline translator fixes the fallback point size at 1.0; shaders
    // that explicitly write PointSize retain that output. Other fixed sizes
    // require a new translation specialization and must not be ignored.
    COMMAND_LOCK;need(as<cmd_t>(c).recording,"Point size outside command recording");
    need(size==1.0f,"Non-default point size requires a shader specialization");
}
API(nvnCommandBufferBarrier, void, NVNcommandBuffer *c, int bits) {
    COMMAND_LOCK;
    record(c, OP_BARRIER, uint32_t(bits));
}

API(nvnCommandBufferSetDepthBounds, void, NVNcommandBuffer *c,uint32_t enabled,float minimum,float maximum) {
    COMMAND_LOCK;
    need(std::isfinite(minimum) && std::isfinite(maximum) && minimum>=0 && maximum<=1 && minimum<=maximum,
         "Invalid depth bounds range");
    const bool active=(enabled&255)!=0;
    need(!active || host_config.enabled_core.depthBounds,"Depth bounds unsupported by Vulkan device");
    record(c,OP_COUNT_+5,xbg_depth_bounds{uint32_t(active),minimum,maximum});
}
API(nvnCommandBufferSetTiledCacheAction, void, NVNcommandBuffer *c, NVNtiledCacheAction action) {
    COMMAND_LOCK;
    switch(action) {
    case NVN_TILED_CACHE_ACTION_ENABLE:
    case NVN_TILED_CACHE_ACTION_DISABLE:
    case NVN_TILED_CACHE_ACTION_FLUSH:
    case NVN_TILED_CACHE_ACTION_FLUSH_NO_TILING:
    case NVN_TILED_CACHE_ACTION_ENABLE_RENDER_TARGET_BINNING:
    case NVN_TILED_CACHE_ACTION_DISABLE_RENDER_TARGET_BINNING:
        // Vulkan owns tile/binning policy. Conservatively finish the current
        // render pass and preserve all memory dependencies at this boundary.
        // OP_BARRIER's Commands scope emits the engine's full GPU barrier.
        record(c,OP_BARRIER,uint32_t(0));
        break;
    default:need(false,"Invalid tiled cache action");
    }
}
API(nvnCommandBufferSetTiledCacheTileSize, void, NVNcommandBuffer *c,int width,int height) {
    COMMAND_LOCK;need(as<cmd_t>(c).recording,"Tile size outside command recording");
    need(width>0 && height>0,"Invalid tiled cache dimensions");
    // Vulkan chooses physical tile dimensions; this does not alter viewport,
    // scissor, attachment extent or the set of fragments that must be drawn.
}

// Program metadata is copied at bind time; shader control/code remain in the
// caller's immutable shader pool. The engine loads matching offline XGS/SPIR-V.
std::map<uint64_t, std::unique_ptr<prog_t>> program_snapshots;
API(nvnProgramInitialize, NVNboolean, NVNprogram *p, NVNdevice *) {
    LOCK;
    as<prog_t>(p) = {};
    as<prog_t>(p).magic = MAGIC('P', 'R', 'G', 0);
    as<prog_t>(p).serial = nvn_new_serial();
    return 1;
}
API(nvnProgramSetShaders, NVNboolean, NVNprogram *p, int n, const NVNshaderData *shaders) {
    LOCK;
    if (n < 1 || n > 6 || !shaders)
        return 0;
    auto &program = as<prog_t>(p);
    program.nstage = n;
    memcpy(program.sd, shaders, n * sizeof(*shaders));
    program.serial = nvn_new_serial();
    return 1;
}
// Extended shader descriptors used by the shared native Zelda frontend.
// Keep the extra word explicit and reject unknown extension data.
struct ShaderDataExt { NVNbufferAddress data;const void *control;uint64_t reserved; };
static_assert(sizeof(ShaderDataExt)==24);
static NVNboolean nvnProgramSetShadersExt(NVNprogram *p,int n,const ShaderDataExt *source) {
    if(n<1 || n>6 || !source)return 0;
    NVNshaderData data[6]{};
    for(int i=0;i<n;++i){need(!source[i].reserved,"Unknown shader extension data");data[i]={source[i].data,source[i].control};}
    return nvnProgramSetShaders(p,n,data);
}
static Register register_ShaderDataExt("nvnProgramSetShadersExt",reinterpret_cast<void*>(nvnProgramSetShadersExt));
API(nvnProgramFinalize, void, NVNprogram *p) {
    LOCK;
    xbg_legacy_finish();
    xbg_legacy_prog_release(&as<prog_t>(p));
    as<prog_t>(p) = {};
}
API(nvnCommandBufferBindProgram, void, NVNcommandBuffer *c, const NVNprogram *p, int stages) {
    COMMAND_LOCK;
    p_bind_program payload{};
    payload.stages = stages;
    if (p) {
        auto &program = as<prog_t>(p);
        need(program.magic == MAGIC('P', 'R', 'G', 0), "Invalid program");
        auto &snapshot = program_snapshots[program.serial];
        if (!snapshot) {
            need(program_snapshots.size() <= 65536, "Program snapshot limit");
            snapshot = std::make_unique<prog_t>(program);
        }
        payload.prog = snapshot.get();
    }
    record(c, OP_BIND_PROGRAM, payload);
}
API(nvnCommandBufferBindUniformBuffer, void, NVNcommandBuffer *c, NVNshaderStage stage, int index,
    NVNbufferAddress address, size_t bytes) {
    COMMAND_LOCK;
    record(c, OP_BIND_UBO, p_bind_buf{stage, index, address, bytes});
}
API(nvnCommandBufferBindStorageBuffer, void, NVNcommandBuffer *c, NVNshaderStage stage, int index,
    NVNbufferAddress address, size_t bytes) {
    COMMAND_LOCK;
    record(c, OP_BIND_SSBO, p_bind_buf{stage, index, address, bytes});
}
API(nvnCommandBufferBindVertexBuffer, void, NVNcommandBuffer *c, int index,
    NVNbufferAddress address, size_t bytes) {
    COMMAND_LOCK;
    need(index >= 0 && index < 16, "Vertex binding");
    p_bind_vbuf p{};
    p.first = index;
    p.n = 1;
    p.r[0] = {address, bytes};
    record(c, OP_BIND_VBUF, p);
}
API(nvnCommandBufferBindVertexBuffers, void, NVNcommandBuffer *c, int first, int n,
    const NVNbufferRange *ranges) {
    COMMAND_LOCK;
    need(first >= 0 && n >= 0 && n <= 16 - first && (!n || ranges), "Vertex binding range");
    p_bind_vbuf p{};
    p.first = first;
    p.n = n;
    if (n)
        memcpy(p.r, ranges, n * sizeof(*ranges));
    record(c, OP_BIND_VBUF, p);
}
API(nvnCommandBufferDrawArrays, void, NVNcommandBuffer *c, NVNdrawPrimitive primitive, int first,
    int count) {
    COMMAND_LOCK;
    need(first >= 0 && count >= 0, "Draw range");
    record(c, OP_DRAW, p_draw{primitive, first, count});
}
API(nvnCommandBufferDrawArraysInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive primitive,
    int first, int count, int base_instance, int instances) {
    COMMAND_LOCK;
    need(first>=0 && count>=0 && base_instance>=0 && instances>=0,"Instanced draw range");
    if(!instances)return;
    // Mac's p_draw is five int32 values plus its aligned command header.
    // Keep that compatible guest budget separate from the backend draw plan.
    record(c,OP_COUNT_+7,xbg_draw_call{primitive,-1,first,count,instances,base_instance,0,0},32);
}
API(nvnCommandBufferDrawElementsInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive primitive,
    NVNindexType type, int count, NVNbufferAddress indices, int base_vertex, int base_instance,
    int instances) {
    COMMAND_LOCK;
    need(count >= 0 && base_instance >= 0 && instances >= 0, "Indexed draw range");
    record(c, OP_DRAW_IDX,
           p_draw_idx{primitive, type, count, base_vertex, base_instance, instances, indices});
}
API(nvnCommandBufferDrawElements, void, NVNcommandBuffer *c, NVNdrawPrimitive primitive,
    NVNindexType type, int count, NVNbufferAddress indices) {
    COMMAND_LOCK;need(count>=0,"Indexed draw range");
    record(c,OP_DRAW_IDX,p_draw_idx{primitive,type,count,0,0,1,indices},48);
}
API(nvnCommandBufferDrawElementsBaseVertex, void, NVNcommandBuffer *c, NVNdrawPrimitive primitive,
    NVNindexType type, int count, NVNbufferAddress indices, int base_vertex) {
    nvnCommandBufferDrawElementsInstanced(c, primitive, type, count, indices, base_vertex, 0, 1);
}
API(nvnCommandBufferDispatchCompute, void, NVNcommandBuffer *c, int x, int y, int z) {
    COMMAND_LOCK;
    need(x >= 0 && y >= 0 && z >= 0, "Dispatch range");
    record(c, OP_DISPATCH, p_dispatch{x, y, z, 0});
}
API(nvnTexturePoolInitialize, NVNboolean, NVNtexturePool *p, const NVNmemoryPool *pool,
    ptrdiff_t offset, int count) {
    LOCK;
    if (count < 0 || !pool || !range(as<pool_t>(pool).size, offset, uint64_t(count) * 32))
        return 0;
    as<dpool_t>(p) = {&as<pool_t>(pool), offset, count};
    return 1;
}
tex_t descriptor_snapshot(const NVNtexture *texture,const NVNtextureView *view) {
    auto source=read_texture(texture);
    need(source.magic==MAGIC('T','E','X',0),"Invalid descriptor texture");
    auto state=read_view(view);
    if(state.flags&16) {
        const std::array<uint8_t,4> components{uint8_t(state.reserved),uint8_t(state.reserved>>8),
            uint8_t(state.reserved>>16),uint8_t(state.reserved>>24)};
        source.serial=(source.serial & ~swizzle_mask) | pack_swizzle(components);
    }
    return source;
}
extern "C" void mng_invalidate_pool_images(const pool_t*);
void invalidate_pool_textures(const pool_t *pool) {mng_invalidate_pool_images(pool);}
API(nvnTexturePoolRegisterTexture, void, const NVNtexturePool *p, int id, const NVNtexture *texture,
    const NVNtextureView *view) {
    if(publications.enabled()){
        Publication operation=RegisterTexture{as<dpool_t>(p),id,descriptor_snapshot(texture,view),translate_view(view,texture),view!=nullptr};
        if(publications.publish(operation))return;
    }
    PUBLISH_LOCK;
    auto v = translate_view(view,texture);
    auto value=descriptor_snapshot(texture,view);const auto pool=as<dpool_t>(p);const bool has_view=view!=nullptr;
    if(translation_owner){translation_owner->submit([pool,id,value,v,has_view]{
        xbg_register_texture(&pool,id,&value,has_view?&v:nullptr);return uint64_t(0);
    },sizeof(pool)+sizeof(value)+sizeof(v));return;}
    xbg_register_texture(&pool, id, &value, has_view ? &v : nullptr);
}
API(nvnSamplerPoolInitialize, NVNboolean, NVNsamplerPool *p, const NVNmemoryPool *pool, ptrdiff_t offset, int count) {
    LOCK;
    if (count < 0 || !pool || !range(as<pool_t>(pool).size, offset, uint64_t(count)*32)) return 0;
    as<dpool_t>(p) = {&as<pool_t>(pool),offset,count}; return 1;
}
void register_sampler(const dpool_t &pool,int id,const samp_par &sampler){
    need(id>=0 && id<pool.n,"Sampler descriptor range");
    if(translation_owner){translation_owner->submit([pool,id,sampler]{
        xbg_register_sampler(&pool,id,&sampler);return uint64_t(0);
    },sizeof(pool)+sizeof(sampler));return;}
    xbg_register_sampler(&pool,id,&sampler);
}
API(nvnSamplerPoolRegisterSampler, void, const NVNsamplerPool *p, int id, const NVNsampler *s) {
    if(publications.enabled()){
        const auto pool=as<dpool_t>(p);need(id>=0 && id<pool.n,"Sampler descriptor range");
        Publication operation=RegisterSampler{pool,id,as<samp_t>(s).p};if(publications.publish(operation))return;
    }
    PUBLISH_LOCK;register_sampler(as<dpool_t>(p),id,as<samp_t>(s).p);
}
API(nvnSamplerPoolRegisterSamplerBuilder, void, const NVNsamplerPool *p, int id, const NVNsamplerBuilder *s) {
    if(publications.enabled()){
        const auto pool=as<dpool_t>(p);need(id>=0 && id<pool.n,"Sampler descriptor range");
        Publication operation=RegisterSampler{pool,id,as<samp_builder_t>(s).p};if(publications.publish(operation))return;
    }
    PUBLISH_LOCK;register_sampler(as<dpool_t>(p),id,as<samp_builder_t>(s).p);
}
API(nvnSamplerBuilderSetDevice, void, NVNsamplerBuilder *b, NVNdevice *d) { as<samp_builder_t>(b).dev=d; }
API(nvnSamplerBuilderSetDefaults, void, NVNsamplerBuilder *b) {
    auto &p=as<samp_builder_t>(b).p; p={};
    p.minf=NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR; p.magf=NVN_MAG_FILTER_LINEAR;
    p.ws=p.wt=p.wr=NVN_WRAP_MODE_REPEAT; p.lod_min=-1000; p.lod_max=1000; p.aniso=1;
    p.cmp_func=NVN_COMPARE_FUNC_LESS;
}
API(nvnSamplerBuilderSetMinMagFilter, void, NVNsamplerBuilder *b, NVNminFilter min, NVNmagFilter mag) {
    auto &p=as<samp_builder_t>(b).p; p.minf=min;p.magf=mag;
}
API(nvnSamplerBuilderSetWrapMode, void, NVNsamplerBuilder *b, NVNwrapMode s, NVNwrapMode t, NVNwrapMode r) {
    auto &p=as<samp_builder_t>(b).p;p.ws=s;p.wt=t;p.wr=r;
}
API(nvnSamplerBuilderSetLodClamp, void, NVNsamplerBuilder *b, float min, float max) {
    auto &p=as<samp_builder_t>(b).p;p.lod_min=min;p.lod_max=max;
}
API(nvnSamplerBuilderSetLodBias, void, NVNsamplerBuilder *b, float bias) { as<samp_builder_t>(b).p.lod_bias=bias; }
API(nvnSamplerBuilderSetCompare, void, NVNsamplerBuilder *b, NVNcompareMode mode, NVNcompareFunc function) {
    auto &p=as<samp_builder_t>(b).p;p.cmp_mode=mode;p.cmp_func=function;
}
API(nvnSamplerBuilderSetBorderColor, void, NVNsamplerBuilder *b, const float *value) {
    need(value,"Null border color");memcpy(as<samp_builder_t>(b).p.border,value,16);
}
API(nvnSamplerBuilderSetMaxAnisotropy, void, NVNsamplerBuilder *b, float value) {
    need(value>=1,"Invalid anisotropy");as<samp_builder_t>(b).p.aniso=value;
}
API(nvnSamplerBuilderSetReductionFilter, void, NVNsamplerBuilder *, NVNsamplerReduction mode) {
    need(mode==NVN_SAMPLER_REDUCTION_AVERAGE,"Min/max sampler filtering is not enabled");
}
API(nvnSamplerBuilderSetLodSnap, void, NVNsamplerBuilder *, float value) {
    need(value==0,"LOD snapping is not supported by Vulkan sampler");
}
API(nvnSamplerInitialize, NVNboolean, NVNsampler *s, const NVNsamplerBuilder *b) {
    as<samp_t>(s)={MAGIC('S','A','M','P'),as<samp_builder_t>(b).p};return 1;
}
API(nvnSamplerCompare, NVNboolean, const NVNsampler *a, const NVNsampler *b) {
    if(!as<samp_t>(a).magic || !as<samp_t>(b).magic)return 0;
    return !memcmp(&as<samp_t>(a).p,&as<samp_t>(b).p,sizeof(samp_par));
}
API(nvnSamplerFinalize, void, NVNsampler *s) {as<samp_t>(s)={};}
API(nvnCommandBufferSetSamplerPool, void, NVNcommandBuffer *c, const NVNsamplerPool *p) {
    COMMAND_LOCK;record(c,OP_SET_SAMP_POOL,p_set_pool{&as<dpool_t>(p)});
}
API(nvnCommandBufferBindImage, void, NVNcommandBuffer *c, NVNshaderStage stage, int index, NVNimageHandle handle) {
    COMMAND_LOCK;p_bind_handles p{};p.stage=stage;p.first=index;p.n=1;p.h[0]=handle;record(c,OP_BIND_IMG,p);
}
API(nvnCommandBufferSetTexturePool, void, NVNcommandBuffer *c, const NVNtexturePool *p) {
    COMMAND_LOCK;
    record(c, OP_SET_TEX_POOL, p_set_pool{&as<dpool_t>(p)});
}
API(nvnCommandBufferBindTexture, void, NVNcommandBuffer *c, NVNshaderStage stage, int index,
    NVNtextureHandle handle) {
    COMMAND_LOCK;
    p_bind_handles p{};
    p.stage = stage;
    p.first = index;
    p.n = 1;
    p.h[0] = handle;
    record(c, OP_BIND_TEX, p);
}

API(nvnQueueBuilderSetDefaults, void, NVNqueueBuilder *b) {
    auto *d = as<queue_builder_t>(b).dev;
    as<queue_builder_t>(b) = {};
    as<queue_builder_t>(b).dev = d;
}
API(nvnQueueBuilderSetDevice, void, NVNqueueBuilder *b, NVNdevice *d) {
    as<queue_builder_t>(b).dev = d;
}
API(nvnQueueBuilderSetFlags, void, NVNqueueBuilder *b, int f) { as<queue_builder_t>(b).flags = f; }
API(nvnQueueBuilderSetCommandMemorySize, void, NVNqueueBuilder *b, size_t bytes) {as<queue_builder_t>(b).cmd_sz=bytes;}
API(nvnQueueBuilderSetComputeMemorySize, void, NVNqueueBuilder *b, size_t bytes) {as<queue_builder_t>(b).comp_sz=bytes;}
API(nvnQueueBuilderSetControlMemorySize, void, NVNqueueBuilder *b, size_t bytes) {as<queue_builder_t>(b).ctl_sz=bytes;}
API(nvnQueueBuilderSetCommandFlushThreshold, void, NVNqueueBuilder *b, size_t bytes) {as<queue_builder_t>(b).flush=bytes;}
API(nvnQueueBuilderGetQueueMemorySize, size_t, const NVNqueueBuilder *) {
    // Queue state and command allocations are host-owned, not guest work memory.
    return 0;
}
API(nvnQueueBuilderSetQueueMemory, void, NVNqueueBuilder *b, void *memory, size_t bytes) {
    as<queue_builder_t>(b).mem=memory;as<queue_builder_t>(b).mem_sz=bytes;
}
API(nvnQueueInitialize, NVNboolean, NVNqueue *q, const NVNqueueBuilder *b) {
    as<queue_t>(q) = {MAGIC('Q', 'U', 'E', 0), as<queue_builder_t>(b).dev, 0};
    return 1;
}
// Probe-only handoff seam; unset in normal execution. Copied into the next
// accepted packet so clearing the hook cannot race the renderer.
void (*ordered_probe_gate)(void*){};void *ordered_probe_user{};
extern "C" void mng_nvn_ordered_probe_drain(){LOCK;}
extern "C" int mng_nvn_ordered_probe_report(uint64_t address){
    LOCK;mng::port::SubmissionInputs input;return mng_nvn_capture_report_output(address,&input);
}
extern "C" void mng_nvn_ordered_probe_enqueue(void (*callback)(void*),void *user){
    PUBLISH_LOCK;need(bool(translation_owner),"Ordered probe owner missing");
    translation_owner->submit([callback,user]{callback(user);return uint64_t(0);});
}
extern "C" int mng_nvn_ordered_probe_gate(void (*callback)(void*),void *user){
    PUBLISH_LOCK;if(!translation_owner)return 0;
    ordered_probe_gate=callback;ordered_probe_user=user;return 1;
}
API(nvnQueueSubmitCommands, void, NVNqueue *q, int n, const NVNcommandHandle *handles) {
    HostWait host_wait;
    LOCK;
    need(n >= 0 && (!n || handles), "Submission arguments");
    std::vector<uint64_t> resolved;resolved.reserve(n);
    std::vector<std::shared_ptr<CommandCopy>> pins;pins.reserve(n);
    {
        std::lock_guard commands(command_mutex);
        for(int i=0;i<n;++i){auto it=command_copies.find(handles[i]);need(it!=command_copies.end(),"Unknown submitted handle");
            pins.push_back(it->second);resolved.push_back(uintptr_t(pins.back()->bytes.data()));}
    }
    if(translation_owner){
        // Capture inherited constants only after earlier translations have
        // published their state. This is CPU completion, not QueueFinish.
        auto inputs=std::make_shared<mng::port::SubmissionInputs>();
        unsigned reason{};mng::port::SubmissionSite site;
        {spatial::LiteTraceScope scope("NVN.Ordered.Capture");
         reason=mng_capture_submission(n,resolved.data(),inputs.get(),&site);}
        size_t bytes=inputs->retained_bytes()+resolved.capacity()*sizeof(uint64_t)+pins.capacity()*sizeof(std::shared_ptr<CommandCopy>);
        for(const auto &pin:pins){if(pin->bytes.size()>(64u<<20)-std::min<size_t>(bytes,64u<<20)){reason=7;break;}bytes+=pin->bytes.size();}
        if(!reason){
            ordered_capture_bytes.fetch_add(inputs->bytes(),std::memory_order_relaxed);
            last_translation=translation_owner->submit([resolved=std::move(resolved),pins=std::move(pins),inputs,gate=ordered_probe_gate,user=ordered_probe_user]{
                if(gate)gate(user);
                static thread_local bool named=false;if(!named){mng_name_thread("mng-translate");named=true;}
                spatial::LiteTraceScope scope("NVN.Ordered.Execute");
                const auto begin=mng_host_clock_ns(0),cpu=mng_thread_cpu_ns();
                const auto result=mng_execute_submission(int(resolved.size()),resolved.data(),inputs.get());
                const auto end_cpu=mng_thread_cpu_ns();
                spatial::LiteTrace::counter("NVN.Ordered.ExecuteUs",(mng_host_clock_ns(0)-begin)/1000);
                if(cpu && end_cpu>=cpu)spatial::LiteTrace::counter("NVN.Ordered.ThreadCpuUs",(end_cpu-cpu)/1000);
                return result;
            },bytes);
            ordered_probe_gate=nullptr;ordered_probe_user=nullptr;
            if(!inputs->reports().empty()){++ordered_report_submits;ordered_report_outputs+=inputs->reports().size();}
            ++ordered_submits;++as<queue_t>(q).submits;return;
        }
        ++ordered_fallbacks;ordered_fallback_reason=reason;ordered_last_failure_site=site;
        if(reason==5){
            auto sub=unsigned(site.input_failure);if(!sub || sub>=ordered_input_counts.size())sub=unsigned(mng::port::InputFailure::Other);
            ++ordered_input_counts[sub];
            if(!ordered_input_first_valid){ordered_input_first=site;ordered_input_first.input_failure=mng::port::InputFailure(sub);ordered_input_first_valid=true;}
        }
        if(reason==4){
            ordered_failure_site=site;
            if(site.opcode<ordered_opcode_counts.size())++ordered_opcode_counts[site.opcode];
            else ++ordered_opcode_overflow;
            spatial::LiteTrace::counter("NVN.Ordered.FallbackOpcode",site.opcode);
            spatial::LiteTrace::counter("NVN.Ordered.FallbackHandle",site.handle);
            spatial::LiteTrace::counter("NVN.Ordered.FallbackOffset",site.offset);
        }
        if(reason<ordered_fallback_counts.size())++ordered_fallback_counts[reason];
        spatial::LiteTrace::counter("NVN.Ordered.Fallback",reason);
    }
    // Explicit submission boundary, in addition to the outer FrontLock drain.
    drain_publications();
    // Unsupported input visibility keeps the original synchronous semantics.
    xbg_submit(n, resolved.data());
    ++as<queue_t>(q).submits;
}
API(nvnQueueFlush, void, NVNqueue *) {
    HostWait wait;PUBLISH_LOCK;
    if(translation_owner){translation_owner->submit([]{mng_nvn_flush();return uint64_t(0);});return;}
    mng_nvn_flush();
}
API(nvnQueueFinish, void, NVNqueue *) {
    HostWait host_wait;
    LOCK;
    xbg_legacy_finish();
}
API(nvnQueueFinalize, void, NVNqueue *q) {
    LOCK;
    xbg_legacy_finish();
    as<queue_t>(q) = {};
}
void write_sync(NVNsync *sync,const sync_t &value) {
    std::lock_guard lock(sync_mutex);auto &old=as<sync_t>(sync);
    if(old.magic==MAGIC('S','Y','N','C') && old.fenced==4)ordered_syncs.erase(old.fence);
    old=value;
}
std::pair<sync_t,OrderedTicket> sync_snapshot(const NVNsync *sync){
    std::lock_guard lock(sync_mutex);const auto value=as<sync_t>(sync);
    if(value.fenced!=4)return {value,{}};
    const auto found=ordered_syncs.find(value.fence);
    need(found!=ordered_syncs.end(),"Unknown ordered sync generation");
    return {value,found->second};
}
API(nvnSyncInitialize, NVNboolean, NVNsync *s, NVNdevice *) {
    write_sync(s,{MAGIC('S', 'Y', 'N', 'C'), 0, 0});
    return 1;
}
API(nvnSyncFinalize, void, NVNsync *s) {write_sync(s,{});}
API(nvnQueueFenceSync, void, NVNqueue *, NVNsync *s, NVNsyncCondition condition, int flags) {
    PUBLISH_LOCK;
    need(condition == NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE && !(flags & ~1u),
         "Unsupported fence mode");
    if(translation_owner){
        // The worker resolves a host completion record, never a Guest pointer
        // that SyncFinalize/reinitialize might already have reused.
        auto ticket=translation_owner->submit([]{return xbg_legacy_last_fence();});
        std::lock_guard lock(sync_mutex);auto &value=as<sync_t>(s);
        if(value.magic==MAGIC('S','Y','N','C') && value.fenced==4)ordered_syncs.erase(value.fence);
        need(next_sync_id<UINT64_MAX && ordered_syncs.size()<65536,"Ordered sync budget exhausted");
        const auto id=++next_sync_id;ordered_syncs.emplace(id,std::move(ticket));
        value={MAGIC('S','Y','N','C'),4,id};return;
    }
    const auto fence=xbg_legacy_last_fence();
    write_sync(s,{MAGIC('S','Y','N','C'),1,fence});
}
API(nvnSyncWait, NVNsyncWaitResult, const NVNsync *s, uint64_t timeout) {
    // Acquire has already waited for this texture's present copy, not for the
    // queue's most recent submission. No engine lock is needed for that fact.
    auto [sync,ticket]=sync_snapshot(s);
    if(sync.fenced==3)return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
    HostWait host_wait;
    mng::port::WaitBudget budget(timeout);
    spatial::LiteTraceScope scope("NVN.SyncWait");
    spatial::LiteTrace::counter("NVN.SyncWait.Target",sync.fence);
    if (!sync.fenced)return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
    bool waited=false;
    if(ticket){
        waited=!ticket.wait(0);
        if(waited){spatial::LiteTraceScope waiting("NVN.SyncWait.Translation");
            if(!ticket.wait(budget.remaining()))return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;}
        sync.fence=ticket.value();
    }
    if (xbg_legacy_wait(sync.fence, 0))
        return waited?NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED:NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
    return xbg_legacy_wait(sync.fence, budget.remaining()) ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED
                                                : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
}
API(nvnWindowBuilderSetDefaults, void, NVNwindowBuilder *b) {
    auto *device=as<win_builder_t>(b).dev;as<win_builder_t>(b)={};
    as<win_builder_t>(b).dev=device;as<win_builder_t>(b).interval=1;
}
API(nvnWindowBuilderSetDevice, void, NVNwindowBuilder *b, NVNdevice *d) {as<win_builder_t>(b).dev=d;}
API(nvnWindowBuilderSetNativeWindow, void, NVNwindowBuilder *b, NVNnativeWindow w) {as<win_builder_t>(b).native=w;}
API(nvnWindowBuilderSetPresentInterval, void, NVNwindowBuilder *b, int interval) {
    need(interval>=0 && interval<=5,"Invalid present interval");as<win_builder_t>(b).interval=interval;
}
API(nvnWindowBuilderSetTextures, void, NVNwindowBuilder *b, int n, NVNtexture *const *textures) {
    need(n>=2 && n<=4 && textures,"Window texture count");auto &v=as<win_builder_t>(b);
    v.ntex=n;for(int i=0;i<n;++i){need(textures[i],"Null window texture");v.tex[i]=textures[i];}
}
API(nvnWindowInitialize, NVNboolean, NVNwindow *w, const NVNwindowBuilder *b) {
    LOCK;auto &v=as<win_builder_t>(b);if(!v.native || v.ntex<2 || v.ntex>4)return 0;
    auto &o=as<win_t>(w);o={};o.magic=MAGIC('W','I','N',0);o.native=v.native;
    o.ntex=v.ntex;o.interval=v.interval;window_display_ticks[&o]=0;window_present_tickets[&o]={};window_translation_tickets[&o]={};for(int i=0;i<v.ntex;++i)o.tex[i]=v.tex[i];return 1;
}
// Match the Mac frontend: presentation schedules image reuse; pacing blocks
// AcquireTexture, outside the frontend lock, rather than the guest's Present.
static uint64_t present_now_ns() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
static void acquire_texture(NVNwindow *window,int *index,NVNsync *sync) {
    uint64_t available{},display_tick{},ticket{};int interval{};OrderedTicket translation;
    {
        PUBLISH_LOCK;auto &v=as<win_t>(window);need(v.ntex && index,"Invalid window acquire");
        *index=v.next;v.next=(v.next+1)%v.ntex;available=v.avail[*index];
        display_tick=window_display_ticks[&v];interval=v.interval;
        ticket=window_present_tickets[&v][*index];
        if(translation_owner)translation=window_translation_tickets[&v][*index];
    }
    const bool display_paced=mng_android_wait_display_tick && mng_android_wait_display_tick(&display_tick,interval);
    spatial::LiteTrace::counter("Native.DisplayPaced",display_paced?1:0);
    if(!display_paced && available>present_now_ns()) {
        spatial::LiteTraceScope pacing_scope("Host.WindowAcquire.Pacing");
        std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(available)));
    }
    // As in Mac's acquire path, only this image's earlier present must have
    // completed. The present job returns after its GPU copy is finished.
    {spatial::LiteTraceScope wait_scope("Host.WindowAcquire.ImageTicket");
     if(translation)ticket=translation.value();mng_nvn_wait_present(ticket);}
    {PUBLISH_LOCK;window_display_ticks[&as<win_t>(window)]=display_tick;}
    if(sync)write_sync(sync,{MAGIC('S','Y','N','C'),3,0});
}
API(nvnWindowAcquireTexture, NVNwindowAcquireTextureResult, NVNwindow *w, NVNsync *sync, int *index) {
    HostWait host_wait;need(sync,"Null acquire sync");acquire_texture(w,index,sync);
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
API(nvnQueueAcquireTexture, NVNqueueAcquireTextureResult, NVNqueue *, NVNwindow *w, int *index) {
    HostWait host_wait;acquire_texture(w,index,nullptr);return NVN_QUEUE_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
API(nvnWindowSetPresentInterval, void, NVNwindow *w, int interval) {
    PUBLISH_LOCK;need(interval>=0 && interval<=5,"Invalid present interval");as<win_t>(w).interval=interval;
}
API(nvnWindowSetCrop, void, NVNwindow *w, int x, int y, int width, int height) {
    PUBLISH_LOCK;need(x>=0 && y>=0 && width>=0 && height>=0,"Invalid present crop");auto &v=as<win_t>(w);
    v.crop[0]=x;v.crop[1]=y;v.crop[2]=width;v.crop[3]=height;
}
extern "C" int mng_game_frame_step(uint64_t,double*) MNG_OPTIONAL_IMPORT;
extern "C" uint64_t mng_game_present_period(int,int) MNG_OPTIONAL_IMPORT;
API(nvnQueuePresentTexture, void, NVNqueue *, NVNwindow *w, int index) {
    HostWait host_wait;
    PUBLISH_LOCK;auto &v=as<win_t>(w);need(index>=0 && index<v.ntex,"Invalid present texture");
    // Publish the next guest step at the guest Present call, as on Mac, before
    // asynchronous host presentation. Keep the game's requested interval intact.
    double step{};
    const int policy=mng_game_frame_step?mng_game_frame_step(present_now_ns(),&step):0;
    auto presented=v;if(policy)presented.interval=policy;
    spatial::LiteTrace::counter("Native.RequestedPresentInterval",v.interval);
    spatial::LiteTrace::counter("Native.GameStepUs",step*1000000.0);
    auto value=read_texture(v.tex[index]);
    if(translation_owner){
        // At most one present request remains unresolved for this window. The
        // existing presenter also bounds actual copy/fence ownership to one.
        for(const auto &ticket:window_translation_tickets[&v])ticket.wait();
        // CPU-backed presentation also needs owned bytes. A drain establishes
        // the previous packet's writes before capture; it is not GPU idle.
        drain_translation();
        auto input=std::make_shared<mng::port::SubmissionInputs>();
        if(!mng_capture_present(&value,input.get())){
            window_translation_tickets[&v][index]=translation_owner->submit([presented,value,input]{
                mng_execute_present(&presented,&value,input.get());return mng_nvn_present_ticket();
            },sizeof(presented)+sizeof(value)+input->retained_bytes());
        }else{
            xbg_present_texture(&presented,&value);
            window_translation_tickets[&v][index]={};
            window_present_tickets[&v][index]=mng_nvn_present_ticket();
        }
    }else{
        xbg_present_texture(&presented,&value);
        window_present_tickets[&v][index]=mng_nvn_present_ticket();
    }
    ++v.presents;
    const auto now=present_now_ns();const uint64_t period=mng_game_present_period?
        mng_game_present_period(v.interval,policy):uint64_t(presented.interval)*1000000000ull/60;
    nvn_pacer pace{v.pace};const auto slot=now+nvn_pace(&pace,now,period);
    v.pace=pace.target;v.avail[index]=period?slot+period:0;
}
API(nvnWindowFinalize, void, NVNwindow *w) {HostWait wait;LOCK;xbg_legacy_finish();auto& v=as<win_t>(w);window_display_ticks.erase(&v);window_present_tickets.erase(&v);window_translation_tickets.erase(&v);v={};}
API(nvnQueueWaitSync, NVNboolean, NVNqueue *, const NVNsync *s) {
    auto [sync,ticket]=sync_snapshot(s);
    if(sync.fenced==3)return 1;
    HostWait host_wait;
    // Later submissions cannot pass this dependency. Translation completion is
    // not GPU completion, and neither may be replaced by unconditional success.
    LOCK;
    need(sync.fenced, "Queue waiting on an unfenced sync");
    if(ticket)sync.fence=ticket.value();
    return xbg_legacy_wait(sync.fence, UINT64_MAX) != 0;
}
std::map<uint64_t,std::string> debug_labels;
void label_object(const void *object,const char *label) {
    PUBLISH_LOCK;need(object,"Null debug label object");const auto id=uintptr_t(object);
    // Diagnostic labels are a bounded cache, never a resource lifetime root.
    if(debug_labels.size()>=4096 && !debug_labels.contains(id))debug_labels.erase(debug_labels.begin());
    if(!label){debug_labels.erase(id);return;}
    size_t bytes=strnlen(label,4096);need(bytes<4096,"Debug label exceeds retention limit");
    debug_labels[id]=std::string(label,bytes);
}
API(nvnDeviceSetDebugLabel, void, NVNdevice *object, const char *label) {label_object(object,label);}
API(nvnQueueSetDebugLabel, void, NVNqueue *object, const char *label) {label_object(object,label);}
API(nvnWindowSetDebugLabel, void, NVNwindow *object, const char *label) {label_object(object,label);}
API(nvnProgramSetDebugLabel, void, NVNprogram *object, const char *label) {label_object(object,label);}
API(nvnMemoryPoolSetDebugLabel, void, NVNmemoryPool *object, const char *label) {label_object(object,label);}
API(nvnTexturePoolSetDebugLabel, void, NVNtexturePool *object, const char *label) {label_object(object,label);}
API(nvnSamplerPoolSetDebugLabel, void, NVNsamplerPool *object, const char *label) {label_object(object,label);}
API(nvnBufferSetDebugLabel, void, NVNbuffer *object, const char *label) {label_object(object,label);}
API(nvnTextureSetDebugLabel, void, NVNtexture *object, const char *label) {label_object(object,label);}
API(nvnSamplerSetDebugLabel, void, NVNsampler *object, const char *label) {label_object(object,label);}
API(nvnCommandBufferSetDebugLabel, void, NVNcommandBuffer *object, const char *label) {label_object(object,label);}
API(nvnSyncSetDebugLabel, void, NVNsync *object, const char *label) {label_object(object,label);}
#include "state_api.inc"
} // namespace
extern "C" int mng_nvn_invalidate_control(int enabled,char* out,unsigned bytes){return control_invalidate(enabled,out,bytes);}
extern "C" int mng_nvn_publication_control(int enabled,char* out,unsigned bytes){return control_publication(enabled,out,bytes);}
extern "C" uint64_t nvn_new_serial() {
    static std::atomic<uint64_t> next{};
    return ++next;
}
extern "C" void xbg_record_command(cmd_t *c, const void *record, uint32_t bytes) {
    COMMAND_LOCK;
    need(bytes >= 8 && !(bytes & 7), "Invalid copied record");
    memcpy(allocate(*c, bytes), record, bytes);
}
int mng_nvn_initialize(const xbg_game_config *config, char *error, size_t bytes) {
    LOCK;
    if (ready)
        return -1;
    custom_border_enabled=false;
    auto result = xbg_game_initialize(config, error, bytes);
    ready = result == 0;
    if (ready){
        live_range_flush=strcmp(getenv("MNG_LIVE_RANGE_FLUSH"),"0")!=0;
        image_flush=strcmp(getenv("MNG_IMAGE_FLUSH"),"0")!=0;flush_image_fast=0;
        invalidate_fast=!strcmp(getenv("MNG_INVALIDATE_FAST"),"1");
        invalidate_clear=invalidate_conflict=invalidate_legacy=invalidate_unknown=0;
        mng::diagnostics::setInvalidateControl(control_invalidate);
        mng::diagnostics::setFlushImageControl(control_image_flush);
        mng::diagnostics::setFrontLockControl(frontlock_control);
        mng::diagnostics::setOrderedInputsControl(ordered_inputs_control);
        mng::diagnostics::setPublicationControl(control_publication);
        {mng::port::FrontLockStats::Snapshot ignored;frontlock_stats.control(2,ignored);
         frontlock_stats.control(!strcmp(getenv("MNG_FRONTLOCK_DIAGNOSTICS"),"1"),ignored);}
        flush_live_fast=flush_live_conflict=flush_legacy=flush_unknown=0;
        host_config = *config;
        ordered_submits=ordered_fallbacks=ordered_capture_bytes=ordered_drains=0;
        ordered_report_submits=ordered_report_outputs=0;ordered_last_failure_site={};
        ordered_fallback_reason=0;last_translation={};
        for(auto &count:ordered_fallback_counts)count=0;
        ordered_opcode_counts={};ordered_opcode_overflow=0;ordered_failure_site={};
        ordered_input_counts={};ordered_input_first={};ordered_input_first_valid=false;
        ++ordered_owner_generation;
        const char *mode=getenv("MNG_ORDERED_TRANSLATION");
        if(mode && !strcmp(mode,"1"))translation_owner=std::make_unique<mng::port::OrderedExecutor>();
        ordered_flush_fallback=bool(translation_owner);
        publications.reset(!translation_owner && !strcmp(getenv("MNG_PUBLICATION_QUEUE"),"1"));
        publication_retired=publication_textures=publication_samplers=0;
        xbg_logf("NVN ordered translation=%d requests=64 bytes=67108864 conservative_inputs=1",bool(translation_owner));
    }
    return result;
}
void *mng_nvn_get_proc_address(const char *name) {
    PUBLISH_LOCK;
    auto it = functions().find(name ? name : "");
    if (it == functions().end())
        return nullptr;
    return it->second;
}
size_t mng_nvn_proc_names(const char **names, size_t capacity) {
    PUBLISH_LOCK;
    size_t i = 0;
    for (auto &[name, _] : functions()) {
        if (names && i < capacity)
            names[i] = name.c_str();
        ++i;
    }
    return i;
}
int mng_nvn_shutdown() {
    invalidate_fast=false;
    PUBLISH_LOCK;
    while(!publications.try_enable(false))std::this_thread::yield();
    drain_publications();
    ++ordered_owner_generation;
    if(translation_owner)translation_owner->close();
    if(ready)xbg_legacy_finish();
    translation_owner.reset();last_translation={};
    {std::lock_guard lock(sync_mutex);ordered_syncs.clear();}
    window_translation_tickets.clear();window_present_tickets.clear();
    mng_render_record_shutdown();
    int result = xbg_game_shutdown();
    if (!result) {
        for(auto &[pool,backing]:pool_backings) {
            if(backing.report_pool)backing.report_pool->alive.store(false,std::memory_order_release);
            if(backing.virtual_range)munmap(pool->mem,pool->size);
            if(backing.fd>=0)close(backing.fd);
        }
        pool_backings.clear();
        debug_labels.clear();texture_initializations=texture_finalizations=0;
        memory_validator=nullptr;host_wrapper=nullptr;
        {std::lock_guard lock(texture_snapshot_mutex);replay_texture_refs.clear();texture_snapshots.clear();texture_snapshot_creations=0;}
        {std::lock_guard commands(command_mutex);
            program_snapshots.clear();recorders.clear();command_copies.clear();
            command_ranges.clear();command_guest_bytes.clear();command_copy_bytes=0;}
        {std::lock_guard objects_lock(object_mutex);objects.clear();}window_display_ticks.clear();
        host_config = {};
        ready = false;
    }
    return result;
}

extern "C" void mng_nvn_set_host_wrapper(void *(*wrapper)(void *, const char *)) {
    std::lock_guard lock(front_mutex);host_wrapper=wrapper;
}

extern "C" void mng_texture_swizzle(uint64_t serial,VkComponentMapping *out) {
    *out={VkComponentSwizzle((serial&7)+1),VkComponentSwizzle(((serial>>3)&7)+1),
          VkComponentSwizzle(((serial>>6)&7)+1),VkComponentSwizzle(((serial>>9)&7)+1)};
}

extern "C" void mng_nvn_set_memory_validator(int (*validator)(uint64_t,size_t)) { memory_validator=validator; }

// Called only by the serialized renderer when applying SetRenderTargets. Keep
// references across submissions, including after a command handle is retired.
// This mutex is independent of front_mutex to preserve the async lock order.
extern "C" void mng_nvn_retain_render_targets(const p_set_rt *targets) {
    std::lock_guard lock(texture_snapshot_mutex);
    TextureRefs refs;
    auto retain=[&](const tex_t *t){
        if(!t)return;
        auto it=texture_snapshots.find(t->serial);
        need(it!=texture_snapshots.end() && it->second.get()==t,"Unknown replay texture snapshot");
        refs.emplace(it->first,it->second);
    };
    for(int i=0;i<targets->n;++i)retain(targets->color[i]);
    retain(targets->depth);
    replay_texture_refs=std::move(refs);
}

extern "C" int mng_engine_resource_stats(uint64_t*,uint64_t*);
int mng_nvn_resource_stats(mng_nvn_resources *out) {
    if(!out)return -1;
    std::unique_lock front(front_mutex,std::try_to_lock);
    if(!front.owns_lock())return -1;
    std::unique_lock commands(command_mutex,std::try_to_lock);
    if(!commands.owns_lock())return -1;
    std::unique_lock snapshots(texture_snapshot_mutex,std::try_to_lock);
    if(!snapshots.owns_lock())return -1;
    uint64_t images{},physical{};if(mng_engine_resource_stats(&images,&physical))return -1;
    std::unique_lock objects_lock(object_mutex,std::try_to_lock);
    if(!objects_lock.owns_lock())return -1;
    *out={objects.size(),texture_snapshots.size(),replay_texture_refs.size(),
          command_copies.size(),command_copy_bytes,debug_labels.size(),
          texture_initializations,texture_finalizations,images,physical};
    std::copy(std::begin(object_counts),std::end(object_counts),out->object_counts);
    return 0;
}
int mng_nvn_resource_snapshot(char *out,unsigned bytes) {
    mng_nvn_resources r{};if(mng_nvn_resource_stats(&r))return -1;
    return snprintf(out,bytes,"schema=mng.nvn.resources.v1\nhost_objects=%llu\ntexture_snapshots=%llu\nreplay_texture_refs=%llu\ncommand_handles=%llu\ncommand_bytes=%llu\ndebug_labels=%llu\ntexture_initializations=%llu\ntexture_finalizations=%llu\nimage_identities=%llu\nphysical_images=%llu\ntexture_storage=guest_inline\nswizzle_storage=serial_bits\nobjects_command=%llu\nobjects_pool=%llu\nobjects_descriptor_pool=%llu\nobjects_program=%llu\nobjects_sync=%llu\nobjects_queue=%llu\nobjects_window=%llu\nobjects_other=%llu\n",
        (unsigned long long)r.host_objects,(unsigned long long)r.texture_snapshots,
        (unsigned long long)r.replay_texture_refs,(unsigned long long)r.command_handles,
        (unsigned long long)r.command_bytes,(unsigned long long)r.debug_labels,
        (unsigned long long)r.texture_initializations,(unsigned long long)r.texture_finalizations,
        (unsigned long long)r.image_identities,(unsigned long long)r.physical_images,
        (unsigned long long)r.object_counts[0],(unsigned long long)r.object_counts[1],
        (unsigned long long)r.object_counts[2],(unsigned long long)r.object_counts[3],
        (unsigned long long)r.object_counts[4],(unsigned long long)r.object_counts[5],
        (unsigned long long)r.object_counts[6],(unsigned long long)r.object_counts[7]);
}

extern "C" void mng_nvn_set_wait_callbacks(int(*away)(),void(*back)(int*)) {
    LOCK;core_away=away;core_back=back;
}

// Internal synthetic-probe entry: must not be used on a running guest session.
// Hold the translation lock while another thread queries and records commands.
extern "C" int mng_nvn_test_registry_progress(const void* opaque) {
    const auto* pool=static_cast<const NVNmemoryPool*>(opaque);
    void* expected=nvnMemoryPoolMap(pool);const auto size=nvnMemoryPoolGetSize(pool);
    std::promise<bool> result;auto done=result.get_future();
    std::unique_lock translation(front_mutex);
    std::thread reader([&]{
        bool ok=nvnMemoryPoolMap(pool)==expected && nvnMemoryPoolGetBufferAddress(pool)==uintptr_t(expected) && nvnMemoryPoolGetSize(pool)==size;
        int alignment{};nvnDeviceGetInteger(nullptr,NVN_DEVICE_INFO_UNIFORM_BUFFER_ALIGNMENT,&alignment);
        ok=ok && alignment==256;
        nvnMemoryPoolFlushMappedRange(pool,size-1,1);
        NVNtextureBuilder builder{};
        nvnTextureBuilderSetDefaults(&builder);
        nvnTextureBuilderSetFormat(&builder,NVN_FORMAT_RGBA8);
        nvnTextureBuilderSetSize2D(&builder,8,8);
        nvnTextureBuilderSetSwizzle(&builder,NVN_TEXTURE_SWIZZLE_B,NVN_TEXTURE_SWIZZLE_G,NVN_TEXTURE_SWIZZLE_R,NVN_TEXTURE_SWIZZLE_A);
        ok=ok && nvnTextureBuilderGetStorageSize(&builder)>=256 && texture_builder(&builder).swizzle==std::array<uint8_t,4>{4,3,2,5};
        NVNcommandBuffer command{};alignas(8) uint8_t control[64]{};
        ok=ok && nvnCommandBufferInitialize(&command,nullptr);
        nvnCommandBufferAddCommandMemory(&command,nullptr,0,4096);
        nvnCommandBufferAddControlMemory(&command,control,sizeof control);
        nvnCommandBufferBeginRecording(&command);
        nvnCommandBufferSetViewport(&command,0,0,8,8);
        NVNcolorState color{};nvnColorStateSetDefaults(&color);nvnCommandBufferBindColorState(&command,&color);
        auto handle=nvnCommandBufferEndRecording(&command);
        std::shared_ptr<CommandCopy> submitted;
        {std::lock_guard lock(command_mutex);submitted=command_copies.at(handle);}
        const auto before=submitted->bytes;const auto address=uintptr_t(submitted->bytes.data());
        // Model the Submit pin across a concurrent replacement and explicit
        // retirement of the public handle. The translator must still read the
        // old bytes, and its range remains valid until the last pin is gone.
        nvnCommandBufferAddControlMemory(&command,control,sizeof control);
        nvnCommandBufferBeginRecording(&command);
        nvnCommandBufferSetViewport(&command,0,0,16,16);
        const auto replacement=nvnCommandBufferEndRecording(&command);
        ok=ok && replacement==handle;
        nvnDeviceFinalizeCommandHandle(nullptr,replacement);nvnCommandBufferFinalize(&command);
        {std::lock_guard lock(command_mutex);
            ok=ok && submitted->bytes==before && command_ranges.at(address)==before.size();
            submitted.reset();ok=ok && !command_ranges.contains(address);}
        result.set_value(ok);
    });
    const bool progressed=done.wait_for(std::chrono::seconds(2))==std::future_status::ready;
    translation.unlock();reader.join();
    return progressed && done.get()?0:-1;
}

extern "C" int mng_nvn_test_flush_ordering(const void* opaque) {
    const auto *pool=static_cast<const NVNmemoryPool*>(opaque);
    const auto bytes=nvnMemoryPoolGetSize(pool);
    const auto address=uintptr_t(nvnMemoryPoolMap(pool))+bytes-1;
    mng_nvn_note_gpu_range(address,1);
    auto probe=[&](bool blocked){
        std::unique_lock translation(front_mutex);
        std::promise<void> entered,finished;
        auto begun=entered.get_future(),done=finished.get_future();
        std::thread reader([&]{entered.set_value();nvnMemoryPoolFlushMappedRange(pool,bytes-1,1);finished.set_value();});
        begun.wait();
        const bool waiting=done.wait_for(std::chrono::milliseconds(blocked?100:2000))==std::future_status::timeout;
        translation.unlock();reader.join();return waiting==blocked;
    };
    auto lease=mng::port::flush_dependencies().retain(address,1,mng::port::LiveRanges::Writeback);
    if(!probe(true))return -1;
    lease.reset();
    // Image authority alone cannot publish into Guest CPU storage. Mixed CPU
    // outputs and unknown ownership must still serialize in either A/B mode.
    auto image=mng::port::flush_dependencies().retain(address,1,mng::port::LiveRanges::Image);
    if(!probe(!live_range_flush.load() || ordered_flush_fallback.load() || !image_flush.load()))return -1;
    for(unsigned kind:{mng::port::LiveRanges::Writeback,mng::port::LiveRanges::Report,mng::port::LiveRanges::Event,mng::port::LiveRanges::Unknown}){
        auto output=mng::port::flush_dependencies().retain(address,1,kind);if(!probe(true))return -1;
    }
    image.reset();
    // Historical use cannot keep an already-retired normal range blocked.
    return probe(!live_range_flush.load() || ordered_flush_fallback.load())?0:-1;
}

// Backend-only probe: event NVN entry points are not yet exposed by this port.
// Exercise the existing command decoder without inventing a Guest API contract.
extern "C" void mng_nvn_test_record_event(void* buffer,uint64_t address,uint32_t value){
    COMMAND_LOCK;
    struct EventPayload {uint64_t address;uint32_t mode,value;};
    record(static_cast<NVNcommandBuffer*>(buffer),OP_SIGNAL_EVENT,EventPayload{address,NVN_EVENT_SIGNAL_MODE_WRITE,value});
}

extern "C" int mng_nvn_test_invalidate_ordering(const void* opaque) {
    const auto *pool=static_cast<const NVNmemoryPool*>(opaque);
    const auto bytes=nvnMemoryPoolGetSize(pool);
    const auto address=uintptr_t(nvnMemoryPoolMap(pool))+bytes-1;
    struct Restore {
        bool fast=invalidate_fast.load(),ranged=range_invalidate_enabled.load();
        ~Restore(){invalidate_fast=fast;range_invalidate_enabled=ranged;}
    } restore;
    auto probe=[&](const NVNmemoryPool* target,bool blocked){
        std::unique_lock translation(front_mutex);
        std::promise<void> entered,finished;
        auto begun=entered.get_future(),done=finished.get_future();
        std::thread reader([&]{entered.set_value();nvnMemoryPoolInvalidateMappedRange(target,bytes-1,1);finished.set_value();});
        begun.wait();
        const bool waiting=done.wait_for(std::chrono::milliseconds(blocked?100:2000))==std::future_status::timeout;
        translation.unlock();reader.join();return waiting==blocked;
    };
    invalidate_fast=false;
    if(!probe(pool,true))return -1;
    invalidate_fast=true;range_invalidate_enabled=true;
    const bool ordered=ordered_flush_fallback.load();
    if(!probe(pool,ordered))return -1;
    auto image=mng::port::flush_dependencies().retain(address,1,mng::port::LiveRanges::Image);
    if(!probe(pool,ordered))return -1;
    for(unsigned kind:{mng::port::LiveRanges::Writeback,mng::port::LiveRanges::Report,mng::port::LiveRanges::Event,mng::port::LiveRanges::Unknown}){
        auto output=mng::port::flush_dependencies().retain(address,1,kind);
        if(!probe(pool,true))return -1;
    }
    image.reset();
    // Retired output no longer blocks; reusing the address restores dependency.
    if(!probe(pool,ordered))return -1;
    auto disjoint=mng::port::flush_dependencies().retain(address-4,1,mng::port::LiveRanges::Writeback);
    if(!probe(pool,ordered))return -1;
    auto reused=mng::port::flush_dependencies().retain(address,1,mng::port::LiveRanges::Writeback);
    if(!probe(pool,true))return -1;
    reused.reset();disjoint.reset();
    // Only flags are inspected before the legacy full-finish path. This value
    // copy does not own or finalize the real pool's backing allocation.
    for(int flag:{NVN_MEMORY_POOL_FLAGS_PHYSICAL,NVN_MEMORY_POOL_FLAGS_VIRTUAL}){
        NVNmemoryPool alias{};as<pool_t>(&alias)=as<pool_t>(pool);as<pool_t>(&alias).flags|=flag;
        const bool blocked=probe(&alias,true);
        {std::lock_guard objects_lock(object_mutex);uint64_t id;memcpy(&id,&alias,sizeof id);objects.erase(id);}
        if(!blocked)return -1;
    }
    range_invalidate_enabled=false;
    return probe(pool,true)?0:-1;
}

// CPU-only range registry. No renderer lock, GPU ownership or guest memory map.
extern "C" size_t mng_nvn_command_available(const void* pointer) {
    std::lock_guard lock(command_mutex);const auto address=uintptr_t(pointer);
    auto it=command_ranges.upper_bound(address);if(it==command_ranges.begin())return 0;--it;
    return address-it->first<it->second?it->second-(address-it->first):0;
}
