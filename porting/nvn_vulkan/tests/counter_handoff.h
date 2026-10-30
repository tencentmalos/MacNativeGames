// SPDX-License-Identifier: MIT
extern "C" int mng_nvn_ordered_probe_report(uint64_t);
namespace {
uint64_t ordered_stat(const char *name){
    char text[4096]{};mng_nvn_ordered_status(text,sizeof text);
    const auto key=std::string(name)+"=";const auto *p=strstr(text,key.c_str());
    need(p,"Missing ordered report statistic");return strtoull(p+key.size(),nullptr,10);
}
void counter_pool_handoff_test(NVNdevice *device,NVNqueue *queue,NVNcommandBuffer *cb){
    void *memory{};need(posix_memalign(&memory,65536,65536)==0,"Counter pool allocation");
    auto *report=static_cast<NVNcounterData*>(memory);
    NVNmemoryPoolBuilder builder{};NVNmemoryPool pool{};
    NVN(nvnMemoryPoolBuilderSetDefaults)(&builder);NVN(nvnMemoryPoolBuilderSetDevice)(&builder,device);
    NVN(nvnMemoryPoolBuilderSetStorage)(&builder,memory,65536);
    NVN(nvnMemoryPoolBuilderSetFlags)(&builder,NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    for(unsigned generation=0;generation<2;++generation){
        need(NVN(nvnMemoryPoolInitialize)(&pool,&builder),"Counter output generation initialize");
        need(!NVN(nvnMemoryPoolInitialize)(&pool,&builder),"Live pool generation replaced");
        need(mng_nvn_ordered_probe_report(uintptr_t(memory)),"Ordinary report output refused");
        need(!mng_nvn_ordered_probe_report(uintptr_t(memory)+1),"Unaligned report accepted");
        need(!mng_nvn_ordered_probe_report(uintptr_t(memory)+65536),"Report past pool end accepted");
        memset(report,0xff,sizeof *report);
        alignas(8) uint8_t control[128]{};
        NVN(nvnCommandBufferAddControlMemory)(cb,control,sizeof control);
        NVN(nvnCommandBufferBeginRecording)(cb);
        NVN(nvnCommandBufferReportCounter)(cb,NVN_COUNTER_TYPE_TIMESTAMP,uintptr_t(report));
        auto handle=NVN(nvnCommandBufferEndRecording)(cb);
        OrderedProbeGate gate;
        NVN(nvnQueueSubmitCommands)(queue,1,&handle);gate.check();
        if(gate.active){
            need(report->timestamp==UINT64_MAX,"Report published before worker execution");
            std::promise<void> started;
            auto finalizer=std::async(std::launch::async,[&]{started.set_value();NVN(nvnMemoryPoolFinalize)(&pool);});
            started.get_future().wait();
            need(finalizer.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout,"Output pool finalized before translation");
            gate.unblock();finalizer.get();
        }else NVN(nvnMemoryPoolFinalize)(&pool);
        need(report->timestamp!=UINT64_MAX && report->counter==0,"Finalize did not retire report writeback");
        need(!mng_nvn_ordered_probe_report(uintptr_t(memory)),"Finalized generation still captured");
    }
    free(memory);
    alignas(16) NVNcounterData unregistered{};
    alignas(8) uint8_t control[128]{};
    const auto before=ordered_stat("ordered_fallback_8");
    NVN(nvnCommandBufferAddControlMemory)(cb,control,sizeof control);NVN(nvnCommandBufferBeginRecording)(cb);
    NVN(nvnCommandBufferReportCounter)(cb,NVN_COUNTER_TYPE_TIMESTAMP,uintptr_t(&unregistered));
    auto handle=NVN(nvnCommandBufferEndRecording)(cb);
    NVN(nvnQueueSubmitCommands)(queue,1,&handle);NVN(nvnQueueFinish)(queue);
    if(ordered_stat("ordered_translation"))need(ordered_stat("ordered_fallback_8")==before+1,"Unregistered output did not fall back");
    need(unregistered.timestamp!=0,"Serial report fallback lost output");
    fprintf(stderr,"[probe] report output generation, delayed finalize, reuse and unregistered fallback passed\n");
}
}
