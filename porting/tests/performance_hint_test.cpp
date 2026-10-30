// SPDX-License-Identifier: MIT
#include "../runtime/performance_hint_queue.h"
#include <cassert>
#include <chrono>
#include <future>
#include <vector>
using namespace mng::port;
struct Backend : HintBackend {
    std::atomic<bool> block{}, entered{}, release{}, fail{}, support{true};
    std::atomic<unsigned> creates{}, reports{}, closes{};
    std::thread::id caller = std::this_thread::get_id();
    bool available() override { assert(std::this_thread::get_id()!=caller); return support; }
    void* create(int32_t tid,int64_t budget) override {
        assert(std::this_thread::get_id()!=caller && tid>0 && budget==16666667);
        ++creates; return new int32_t(tid);
    }
    bool report(void*,int64_t elapsed) override {
        assert(std::this_thread::get_id()!=caller && elapsed>0);
        ++reports; entered=true;
        while(block && !release) std::this_thread::yield();
        return !fail;
    }
    void close(void* p) override { assert(std::this_thread::get_id()!=caller); ++closes; delete static_cast<int32_t*>(p); }
};
template<class F> HintSnapshot wait(HintQueue& q,F predicate) {
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    HintSnapshot s;
    while(std::chrono::steady_clock::now()<end) {
        if(q.snapshot(s) && predicate(s)) return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(false && "Hint worker failed to reach expected state"); return s;
}
int main() {
    Backend backend;
    {
        HintQueue q(backend,16666667);
        q.submit(HintLane::Translate,1,23000000);
        assert(!q.enabled() && !backend.creates && !backend.reports);
        q.request(true);
        q.submit(HintLane::Translate,1,23000000);
        q.submit(HintLane::Present,2,1000000);
        wait(q,[](auto s){return s.tids[0]==1 && s.tids[1]==2 && s.reported==2;});
        q.submit(HintLane::Translate,3,24000000);
        wait(q,[](auto s){return s.tids[0]==3 && s.reported==3;});
        assert(backend.creates==3 && backend.closes==1);
        backend.block=true; backend.entered=false;
        q.submit(HintLane::Translate,3,23000000);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(!backend.entered && std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
        assert(backend.entered);
        auto producer=std::async(std::launch::async,[&]{
            for(unsigned i=0;i<10000;++i)q.submit(HintLane::Translate,3,23000000);
            q.request(false);
        });
        assert(producer.wait_for(std::chrono::seconds(1))==std::future_status::ready);
        producer.get();
        auto pending=wait(q,[](auto s){return !s.requested;});
        assert(pending.applied!=pending.generation && pending.coalesced>=9999);
        backend.release=true;
        wait(q,[](auto s){return s.applied==s.generation && !s.tids[0] && !s.tids[1];});
        // Error closes the exact owned session; no retry on every frame.
        backend.block=false; backend.fail=true;
        q.request(true);q.submit(HintLane::Translate,4,100);
        wait(q,[](auto s){return s.failures==1;});
        unsigned created=backend.creates;
        q.submit(HintLane::Translate,4,100);
        q.request(false);
        wait(q,[](auto s){return s.applied==s.generation;});
        assert(backend.creates==created);
        backend.fail=false;q.request(true);q.submit(HintLane::Translate,4,100);
        wait(q,[](auto s){return s.tids[0]==4;});
    }
    assert(backend.creates==backend.closes);
    Backend unsupported;unsupported.support=false;
    {HintQueue q(unsupported,16666667);q.request(true);q.submit(HintLane::Translate,5,100);
     wait(q,[](auto s){return s.supported==0;});}
    assert(!unsupported.creates && !unsupported.closes);
}
