// SPDX-License-Identifier: MIT
#include <future>
extern "C" int mng_nvn_ordered_probe_gate(void(*)(void*),void*);
extern "C" void mng_nvn_ordered_probe_drain();
extern "C" void mng_nvn_ordered_probe_enqueue(void(*)(void*),void*);
extern "C" int mng_nvn_ordered_status(char*,unsigned);
namespace {
struct OrderedProbeGate {
    std::promise<void> entered,release;
    std::shared_future<void> gate=release.get_future().share();
    bool active{},released{};
    static void wait(void *user){auto &g=*static_cast<OrderedProbeGate*>(user);g.entered.set_value();g.gate.wait_for(std::chrono::seconds(5));}
    OrderedProbeGate(){active=mng_nvn_ordered_probe_gate(wait,this);}
    void unblock(){if(!released){release.set_value();released=true;}}
    void check(){if(active && entered.get_future().wait_for(std::chrono::seconds(1))!=std::future_status::ready){
        char status[2048]{};mng_nvn_ordered_status(status,sizeof status);fprintf(stderr,"[probe] %s",status);
        need(false,"Submission unexpectedly fell back instead of capturing owned inputs");}}
    ~OrderedProbeGate(){unblock();mng_nvn_ordered_probe_gate(nullptr,nullptr);mng_nvn_ordered_probe_drain();}
};
void ordered_fence_test(NVNdevice *device,NVNqueue *queue,OrderedProbeGate &gate){
    if(!gate.active)return;
    NVNsync sync{};need(NVN(nvnSyncInitialize)(&sync,device),"Ordered sync initialize");
    NVN(nvnQueueFenceSync)(queue,&sync,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    need(NVN(nvnSyncWait)(&sync,0)==NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED,"Translation-pending poll signaled early");
    const auto start=std::chrono::steady_clock::now();
    need(NVN(nvnSyncWait)(&sync,1000000)==NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED,"Translation-pending finite wait signaled early");
    need(std::chrono::steady_clock::now()-start<std::chrono::seconds(1),"Translation wait ignored timeout");
    // Reusing Guest sync storage must never become a worker write destination.
    NVN(nvnSyncFinalize)(&sync);NVN(nvnSyncInitialize)(&sync,device);
    auto drain=std::async(std::launch::async,[]{mng_nvn_ordered_probe_drain();});
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(1);
    bool waiting=false;
    while(std::chrono::steady_clock::now()<deadline){
        char status[4096]{};mng_nvn_ordered_status(status,sizeof status);
        if(strstr(status,"ordered_waiters=1\n")){waiting=true;break;}
        std::this_thread::yield();
    }
    need(waiting,"Drain retained the frontend lock while waiting");
    struct SecondGate {std::promise<void> entered,release;std::shared_future<void> ready=release.get_future().share();} second;
    mng_nvn_ordered_probe_enqueue([](void *p){auto &g=*static_cast<SecondGate*>(p);g.entered.set_value();g.ready.wait_for(std::chrono::seconds(2));},&second);
    gate.unblock();
    need(second.entered.get_future().wait_for(std::chrono::seconds(1))==std::future_status::ready,"New publication did not execute");
    need(drain.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout,"Drain missed publication during unlocked wait");
    second.release.set_value();drain.get();
    NVN(nvnQueueFinish)(queue);
    need(NVN(nvnSyncWait)(&sync,0)==NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED,"Old translation signaled a reused sync");
    NVN(nvnQueueFenceSync)(queue,&sync,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    NVN(nvnQueueFlush)(queue);
    need(NVN(nvnQueueWaitSync)(queue,&sync),"Ordered queue dependency failed");
    need(NVN(nvnSyncWait)(&sync,UINT64_MAX)==NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED,"Completed ordered fence lost");
    NVN(nvnSyncFinalize)(&sync);
}
}
