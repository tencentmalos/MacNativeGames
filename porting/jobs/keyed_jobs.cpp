// SPDX-License-Identifier: MIT
#include "keyed_jobs.h"
#include <spatial/async_task/jobs/JobSystem.hpp>
#include <spatial/core/utils/LiteTrace.h>
#include <spatial/core/utils/ProfilerRing.h>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <unordered_map>

namespace mng::jobs {
struct KeyedJobs::Impl {
    enum class State {queued, running, ready, failed, cancelled};
    struct Entry {
        State state=State::queued;
        std::function<bool()> build;
        uint64_t cookie=0, generation=0;
        size_t bytes=0;
    };
    spatial::jobs::JobSystem system;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::unordered_map<std::string,std::shared_ptr<Entry>> entries;
    Stats counters;
    size_t max_tasks,max_bytes;
    bool stopped=false;
    Impl(unsigned workers,size_t tasks,size_t bytes):max_tasks(tasks),max_bytes(bytes) {
        // This owner uses only WORK_JOB; do not reassign the guest/renderer
        // thread as Foundation logic or require a logic-thread event loop.
        system.addNewSlot(spatial::JobType::WORK_JOB,int(std::clamp(workers,1u,8u)));
    }
    void end_queue(Entry& e) {
        if(e.cookie && e.generation==spatial::ProfilerRing::Generation())
            spatial::LiteTrace::regionEnd(e.cookie);
        e.cookie=0;
    }
    void execute(const std::shared_ptr<Entry>& e,bool consumer) {
        {
            std::lock_guard lock(mutex);
            if(stopped || e->state!=State::queued)return;
            e->state=State::running;end_queue(*e);
            ++counters.active;counters.peak=std::max(counters.peak,counters.active);
            if(consumer)++counters.claimed;
        }
        bool ok=false;
        {
            spatial::LiteTraceScope scope("NVN.PSO.Build");
            spatial::LiteTraceRegionScope region("NVN.PSO.Build");
            try {ok=e->build();}catch(...){ok=false;}
        }
        {
            std::lock_guard lock(mutex);
            e->build={};counters.bytes-=e->bytes;e->bytes=0;
            e->state=ok?State::ready:State::failed;
            --counters.active;if(ok)++counters.built;else ++counters.failed;
        }
        cv.notify_all();
    }
};
KeyedJobs::KeyedJobs(unsigned workers,size_t tasks,size_t bytes):impl(std::make_unique<Impl>(workers,tasks,bytes)){}
KeyedJobs::~KeyedJobs(){stop();}
bool KeyedJobs::enqueue(std::string key,size_t bytes,std::function<bool()> build){
    std::lock_guard lock(impl->mutex);
    if(impl->stopped)return false;
    if(impl->entries.count(key)){++impl->counters.deduplicated;return true;}
    // Account the retained key even after the callback's input is released.
    if(impl->entries.size()>=impl->max_tasks || key.size()>impl->max_bytes ||
       bytes>impl->max_bytes-key.size() || impl->counters.bytes>impl->max_bytes-bytes-key.size()){
        ++impl->counters.rejected;return false;
    }
    auto e=std::make_shared<Impl::Entry>();e->build=std::move(build);e->bytes=bytes;
    if(spatial::ProfilerRing::Enabled()){
        e->generation=spatial::ProfilerRing::Generation();
        e->cookie=spatial::LiteTrace::regionCookieCreate();
        spatial::LiteTrace::regionBegin("NVN.PSO.Queued",e->cookie);
    }
    impl->counters.bytes+=bytes+key.size();++impl->counters.submitted;
    impl->entries.emplace(std::move(key),e);
    impl->system.post([p=impl.get(),e]{p->execute(e,false);},spatial::JobType::WORK_JOB);
    return true;
}
bool KeyedJobs::acquire(const std::string& key){
    std::unique_lock lock(impl->mutex);
    auto it=impl->entries.find(key);if(it==impl->entries.end() || impl->stopped)return false;
    auto e=it->second;
    if(e->state==Impl::State::queued){lock.unlock();impl->execute(e,true);lock.lock();}
    if(e->state==Impl::State::running){
        ++impl->counters.waits;
        spatial::LiteTraceScope scope("NVN.PSO.Wait");
        impl->cv.wait(lock,[&]{return e->state!=Impl::State::running;});
    }
    return e->state==Impl::State::ready;
}
KeyedJobs::Stats KeyedJobs::stats()const{std::lock_guard lock(impl->mutex);return impl->counters;}
void KeyedJobs::stop(){
    {
        std::lock_guard lock(impl->mutex);if(impl->stopped)return;impl->stopped=true;
        for(auto& [key,e]:impl->entries)if(e->state==Impl::State::queued){
            impl->end_queue(*e);e->state=Impl::State::cancelled;e->build={};
            impl->counters.bytes-=e->bytes;e->bytes=0;++impl->counters.cancelled;
        }
    }
    impl->system.stopSlot(spatial::JobType::WORK_JOB);
    {std::unique_lock lock(impl->mutex);impl->cv.wait(lock,[&]{return impl->counters.active==0;});}
    impl->system.destroy();
}
}
