// SPDX-License-Identifier: MIT
// Synthetic GPU regression: no game data, window or timing-dependent workload.
#include <future>
namespace {
struct SyncWaitGate {
    static inline SyncWaitGate *active{};
    PFN_vkGetFenceStatus status=vkGetFenceStatus;
    PFN_vkWaitForFences wait=vkWaitForFences;
    std::atomic<bool> pending{true},armed{true};
    std::promise<void> entered,release;
    std::shared_future<void> gate=release.get_future().share();
    static VKAPI_ATTR VkResult VKAPI_CALL get_status(VkDevice d,VkFence f) {
        return active->pending.load()?VK_NOT_READY:active->status(d,f);
    }
    static VKAPI_ATTR VkResult VKAPI_CALL wait_fences(VkDevice d,uint32_t n,const VkFence *f,VkBool32 all,uint64_t timeout) {
        auto &g=*active;
        if(!timeout && g.pending.load())return VK_TIMEOUT;
        if(timeout && g.armed.exchange(false)) {
            g.entered.set_value();
            g.gate.wait_for(std::chrono::seconds(5));
            g.pending=false;
        }
        return g.wait(d,n,f,all,timeout);
    }
    SyncWaitGate(){active=this;vkGetFenceStatus=get_status;vkWaitForFences=wait_fences;}
    ~SyncWaitGate(){vkGetFenceStatus=status;vkWaitForFences=wait;active=nullptr;}
};
void range_invalidate_test(NVNdevice *device,NVNqueue *queue,NVNmemoryPool *pool){
    NVN(nvnQueueFinish)(queue);
    NVNcommandBuffer cb{};alignas(8) uint8_t control[128]{};
    need(NVN(nvnCommandBufferInitialize)(&cb,device),"Range invalidate recorder");
    NVN(nvnCommandBufferAddControlMemory)(&cb,control,sizeof control);
    constexpr ptrdiff_t offset=3u<<20;
    auto *report=reinterpret_cast<NVNcounterData*>(static_cast<uint8_t*>(NVN(nvnMemoryPoolMap)(pool))+offset);
    *report={};
    NVN(nvnCommandBufferBeginRecording)(&cb);
    NVN(nvnCommandBufferReportCounter)(&cb,NVN_COUNTER_TYPE_TIMESTAMP_TOP,uintptr_t(report));
    auto handle=NVN(nvnCommandBufferEndRecording)(&cb);
    NVN(nvnQueueSubmitCommands)(queue,1,&handle);NVN(nvnQueueFlush)(queue);
    mng_nvn_ordered_probe_drain(); // Install dispatch hooks only after CPU translation.
    bool progressed=false,blocked=false;
    {
        SyncWaitGate gate;
        auto entered=gate.entered.get_future();
        auto independent=std::async(std::launch::async,[&]{NVN(nvnMemoryPoolInvalidateMappedRange)(pool,offset+64,16);});
        progressed=independent.wait_for(std::chrono::seconds(1))==std::future_status::ready;
        if(!progressed){gate.release.set_value();independent.get();}
        else {
            independent.get();
            auto dependent=std::async(std::launch::async,[&]{NVN(nvnMemoryPoolInvalidateMappedRange)(pool,offset,sizeof(*report));});
            blocked=entered.wait_for(std::chrono::seconds(1))==std::future_status::ready &&
                dependent.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout;
            gate.release.set_value();dependent.get();
        }
    }
    need(progressed,"Unrelated CPU invalidation waited for a GPU report");
    need(blocked && report->timestamp,"Overlapping CPU invalidation skipped report completion");
    NVN(nvnDeviceFinalizeCommandHandle)(device,handle);NVN(nvnCommandBufferFinalize)(&cb);
    fprintf(stderr,"[probe] range invalidation progresses past unrelated GPU work; overlapping timestamp waits and becomes visible\n");
}
void sync_wait_progress_test(NVNdevice *device,NVNqueue *queue,NVNtexture *texture) {
    // Start with empty slots so the independent submission does not need to
    // recycle the slot pinned by the waiter. Slot exhaustion is real backpressure.
    NVN(nvnQueueFinish)(queue);
    NVNcommandBuffer cb[2]{};alignas(8) uint8_t control[2][128]{};
    NVNcommandHandle handles[2]{};
    for(unsigned i=0;i<2;++i) {
        need(NVN(nvnCommandBufferInitialize)(&cb[i],device),"Sync progress recorder");
        NVN(nvnCommandBufferAddControlMemory)(&cb[i],control[i],sizeof control[i]);
        NVN(nvnCommandBufferBeginRecording)(&cb[i]);
        const float color[]{float(!i),float(i),0,1};NVNcopyRegion region{0,0,0,64,32,1};
        NVN(nvnCommandBufferClearTexture)(&cb[i],texture,nullptr,&region,color,15);
        handles[i]=NVN(nvnCommandBufferEndRecording)(&cb[i]);
    }
    NVNsync first{},later{};
    need(NVN(nvnSyncInitialize)(&first,device) && NVN(nvnSyncInitialize)(&later,device),"Sync progress objects");
    NVN(nvnQueueSubmitCommands)(queue,1,&handles[0]);
    NVN(nvnQueueFenceSync)(queue,&first,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
    NVN(nvnQueueFlush)(queue);
    mng_nvn_ordered_probe_drain();
    bool advanced=false,entered=false;NVNsyncWaitResult result{};
    {
        SyncWaitGate audit;
        need(NVN(nvnSyncWait)(&first,0)==NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED,"Pending zero-timeout fence must not signal");
        need(audit.armed,"Zero-timeout poll used a blocking GPU wait");
        auto waiting=std::async(std::launch::async,[&]{return NVN(nvnSyncWait)(&first,5000000000ull);});
        entered=audit.entered.get_future().wait_for(std::chrono::seconds(1))==std::future_status::ready;
        auto submit=std::async(std::launch::async,[&]{
            NVN(nvnQueueSubmitCommands)(queue,1,&handles[1]);
            NVN(nvnQueueFenceSync)(queue,&later,NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE,0);
        });
        advanced=submit.wait_for(std::chrono::seconds(1))==std::future_status::ready;
        audit.release.set_value();result=waiting.get();submit.get();
    }
    need(entered,"Synthetic fence wait did not enter the bounded gate");
    need(advanced,"CPU SyncWait blocked independent command translation");
    need(result==NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || result==NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED,"Target fence failed");
    need(NVN(nvnSyncWait)(&later,0)==NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED,"An older fence completed a later unflushed submission");
    NVN(nvnQueueFlush)(queue);
    result=NVN(nvnSyncWait)(&later,5000000000ull);
    need(result==NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED || result==NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED,"Later fence failed");
    NVNcopyRegion region{0,0,0,64,32,1};std::vector<uint8_t> pixels(64*32*4);
    NVN(nvnTextureReadTexels)(texture,nullptr,&region,pixels.data());
    for(unsigned i=0;i<pixels.size();i+=4)need(pixels[i]==0 && pixels[i+1]==255 && pixels[i+2]==0 && pixels[i+3]==255,"Concurrent submission order/readback changed");
    for(unsigned i=0;i<2;++i){NVN(nvnDeviceFinalizeCommandHandle)(device,handles[i]);NVN(nvnCommandBufferFinalize)(&cb[i]);}
    NVN(nvnSyncFinalize)(&first);NVN(nvnSyncFinalize)(&later);
    fprintf(stderr,"[probe] CPU fence wait allows next translation; zero-timeout and target fence ordering; 2048 pixels verified\n");
}
}
