// SPDX-License-Identifier: MIT
#include "profiler.h"
#include "profile_c.h"
#include "guest_tags.h"
#include "debugbus.h"
#include <array>
#include <cstdio>
#include "RingVulkanGpuBridge.hpp"
#include <atomic>
#include <charconv>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>
#include <thread>
#include <chrono>
#include <stdexcept>
#include <profiler/sdk/Sdk.h>

namespace {std::atomic<uint64_t> guest_epoch{0},address_epoch{0};}
extern "C" {
unsigned mng_profile_hle_active=0;
uint64_t mng_profile_guest_epoch(){
    const auto epoch=guest_epoch.load(std::memory_order_acquire);
    return (epoch&1) && spatial::ProfilerRing::Enabled()?epoch:0;
}
void mng_profile_guest_event(const char* name,uint64_t epoch,uint64_t token,
                             uint64_t wait_id,uint32_t waiter,uint32_t peer,int64_t result){
    if(!epoch || epoch!=mng_profile_guest_epoch() || !token || token-1!=spatial::ProfilerRing::Generation())return;
    const auto scope=mng_profile_begin(name);
    mng_profile_counter("Guest.Condition.Epoch",epoch);
    mng_profile_counter("Guest.Condition.WaitId",wait_id);
    mng_profile_counter("Guest.Condition.Waiter",waiter);
    mng_profile_counter("Guest.Condition.Peer",peer);
    mng_profile_counter("Guest.Condition.Result",result);
    mng_profile_end(scope);
}
uint64_t mng_profile_address_epoch(){
    const auto epoch=address_epoch.load(std::memory_order_acquire);
    return (epoch&1) && spatial::ProfilerRing::Enabled()?epoch:0;
}
void mng_profile_address_event(const char* name,uint64_t epoch,uint64_t token,
    uint64_t wait_id,uint64_t address,uint32_t waiter,uint32_t peer,int64_t result){
    if(!epoch || epoch!=mng_profile_address_epoch() || !token || token-1!=spatial::ProfilerRing::Generation())return;
    const auto scope=mng_profile_begin(name);
    mng_profile_counter("Guest.Address.Epoch",epoch);mng_profile_counter("Guest.Address.WaitId",wait_id);
    mng_profile_counter("Guest.Address.Object",address);mng_profile_counter("Guest.Address.Waiter",waiter);
    mng_profile_counter("Guest.Address.Peer",peer);mng_profile_counter("Guest.Address.Result",result);
    mng_profile_end(scope);
}
uint64_t mng_profile_begin(const char* name){
    if(!spatial::ProfilerRing::Enabled())return 0;
    const auto generation=spatial::ProfilerRing::Generation();spatial::LiteTrace::begin(name);return generation+1;
}
void mng_profile_end(uint64_t token){if(token && token-1==spatial::ProfilerRing::Generation())spatial::LiteTrace::end();}
mng_profile_flow mng_profile_post(const char* name){
    if(!spatial::ProfilerRing::Enabled())return {};
    static std::atomic<uint64_t> next{1};
    const auto generation=spatial::ProfilerRing::Generation();
    const auto id=next.fetch_add(1,std::memory_order_relaxed);
    spatial::LiteTrace::post(name,id);
    return {id,generation+1};
}
uint64_t mng_profile_flow_begin(const char* name,mng_profile_flow flow){
    if(!spatial::ProfilerRing::Enabled())return 0;
    const auto generation=spatial::ProfilerRing::Generation();
    if(flow.id && flow.generation==generation+1)spatial::LiteTrace::beginFlow(name,flow.id);
    else spatial::LiteTrace::begin(name);
    return generation+1;
}
void mng_profile_counter(const char* name,int64_t value){spatial::LiteTrace::counter(name,value);}
}
namespace mng::profile {
namespace {
std::atomic<bool> ready{false};
std::atomic<uint32_t> detail_stride{0};
thread_local uint32_t detail_depth=0;
thread_local uint64_t detail_draws=0;
thread_local bool detail_selected=false;
// Packed immutable mode per command; only render owner writes per-frame counts.
std::atomic<uint64_t> fine_gpu{0};
uint64_t fine_seen{},fine_selected{},fine_rejected{},fine_sampled_out{},draw_seen{};
spatial::VulkanGPUProfileInitParam gpu_config;
std::unique_ptr<spatial::modules::RingVulkanGpuBridge> gpu;
std::map<const char*,spatial::ProfilerMethodInfo> methods;
std::mutex status_mutex;
std::string gpu_status="ring_gpu_configured=0\nring_gpu_context_active=0\n";
uint64_t presents{};
Backend active_backend=Backend::Vulkan;
void cacheGpuStatus(){
    if(!gpu)return;
    auto text=gpu->statusText();
    std::lock_guard lock(status_mutex);gpu_status=std::move(text);
}
bool number(const std::string& s,uint32_t& n,uint32_t maximum){
    const auto [end,err]=std::from_chars(s.data(),s.data()+s.size(),n);
    return err==std::errc() && end==s.data()+s.size() && n<=maximum;
}
}
void initialize(const std::string& process,const std::string& directory,const std::string& identity,Backend backend){
    active_backend=backend;
    std::filesystem::create_directories(directory);
    spatial::ProfilerRing::Initialize(process,directory,false,8*1024*1024);
    spatial::ProfilerRing::SetAppInfo(identity+(backend==Backend::Vulkan?
        " frame_source=successful_native_present guest_frame_identity=unknown cpu_scopes=elapsed gpu_queries=submission,upload,present;fine_optional":
        " frame_source=metal_drawable_presented guest_frame_identity=present_ticket cpu_scopes=elapsed gpu_queries=unsupported"));
    if(backend==Backend::Metal)gpu_status="ring_gpu_configured=0\nring_gpu_context_active=0\nreason=metal_gpu_bridge_unavailable\n";
    diagnostics::setIdentity(identity+"\nidentity_source=expected_manifest;query_guest_modules_for_verified_identity\n");
    if(getenv("MNG_PROFILE_STARTUP") && !strcmp(getenv("MNG_PROFILE_STARTUP"),"1")){
        // A real startup capture must include work before the first present.
        // Apply the SDK's cold control transition without inventing a frame or
        // assigning the SDK main-thread identity to this initialization thread.
        spatial::ProfilerRing::StartFileCapture(64,60);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        bool started=false;
        while(std::chrono::steady_clock::now()<deadline){
            const auto status=spatial::ProfilerRing::CaptureStatus();
            if(status.find("capture_state=recording\n")!=std::string::npos){started=true;break;}
            if(status.find("capture_state=failed\n")!=std::string::npos)break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if(!started){spatial::ProfilerRing::StopCapture();throw std::runtime_error("Startup profiler did not start");}
        ::profiler::sdk::apply_mode_transition();
    }
    ready.store(true,std::memory_order_release);
}
const char* svcName(unsigned svc){
    static const auto names=[] {std::array<std::array<char,20>,256> value{};
        for(unsigned i=0;i<value.size();++i)snprintf(value[i].data(),value[i].size(),"HLE.SVC.0x%02X",i);return value;}();
    return svc<names.size()?names[svc].data():"HLE.SVC.Unknown";
}
bool detailedCpu(){return detail_depth?detail_selected:detail_stride.load(std::memory_order_relaxed)!=0;}
CpuDetailDrawScope::CpuDetailDrawScope():previous_selected(detail_selected){
    const auto stride=detail_stride.load(std::memory_order_relaxed);
    if(!detail_depth)detail_selected=stride && ++detail_draws%stride==0;
    ++detail_depth;
}
CpuDetailDrawScope::~CpuDetailDrawScope(){--detail_depth;detail_selected=previous_selected;}
std::string command(const std::string& text){
    if(text.size()>256)return "error=command_too_long\n";
    if(!ready.load(std::memory_order_acquire))return "error=profiler_ring_not_initialized\n";
    std::istringstream input(text);std::vector<std::string> args;std::string s;while(input>>s)args.push_back(s);
    if(args.empty())return "error=missing_command\n";
    if(args[0]=="profiler_ring_gpu" && args.size()==1){
        // This lock only copies cached text; GPU queries/calibration never hold it.
        std::unique_lock lock(status_mutex,std::try_to_lock);if(!lock.owns_lock())return "error=profiler_status_busy\nretryable=1\n";return gpu_status;
    }
    const auto action=args.size()>1?args[1]:"status";
    if(args[0]=="profiler_coverage" && args.size()==1){
        if(active_backend==Backend::Metal)return "schema=mng.profiler-coverage.v1\nbackend=metal\ncpu_basic=SVC,guest_wait,core_reacquire,callback,NVN_submit,NVN_translation,Metal_encode_burst,Metal_encode_join\nasync_flows=Metal_pass_publish_to_encoder_begin\ncpu_hle=import_wrapper;opt_in\ngpu_basic=unsupported\ngpu_detail=unsupported\nsource_frame=metal_drawable_presented\nunknown=metal_gpu_timestamps,on_cpu_vs_sleep,hardware_utilization\n";
        return "schema=mng.profiler-coverage.v1\ncpu_basic=NVN_draw,dispatch,submit,fence,present,SVC,guest_wait,core_reacquire,callback\n"
            "cpu_detail=descriptor,snapshot,image,pipeline_lookup,hash,counter_read,draw_encode;profiler_detail_start_stride\n"
            "cpu_guest=condition_svc_1c_1d_module_offset,condition_selected_notify;profiler_guest_start\n"
            "cpu_guest_tags=botw160_root,gsys_phases,worker_mgr;startup_MNG_GUEST_TAGS_1;profiler_tags_start;default_off\n"
            "cpu_hle=import_wrapper;opt_in\ngpu_basic=submission,upload_prologue,present\n"
            "gpu_detail=draw,dispatch,copy,clear,blit;sampled;opt_in\n"
            "pipeline_compile=real_vkCreateGraphicsPipelines,vkCreateComputePipelines\n"
            "gpu_clock_alignment=unknown\nsource_frame=successful_native_present\n"
            "unknown=guest_pure_instructions,signal_handler_preemption,on_cpu_vs_sleep,KGSL,hardware_utilization\n";
    }
    if(args[0]=="profiler_tags" && args.size()<=2)return guestTagsCommand(args.size()==2?args[1]:"status");
    if(args[0]=="profiler_address"){
        if(args.size()>2 || (action!="start" && action!="stop" && action!="status"))return "error=invalid_address_action\n";
        if(action!="status"){
            auto previous=address_epoch.load(std::memory_order_acquire);const bool enabled=action=="start";
            while(bool(previous&1)!=enabled && !address_epoch.compare_exchange_weak(previous,previous+1,std::memory_order_acq_rel)){}
        }
        const auto epoch=address_epoch.load(std::memory_order_acquire);
        return "address="+std::to_string(epoch&1)+"\naddress_epoch="+std::to_string(epoch)+"\ncoverage=address_enqueue_selected_notify_wake_resume\n";
    }
    if(args[0]=="profiler_guest"){
        if(args.size()>2 || (action!="start" && action!="stop" && action!="status"))return "error=invalid_guest_action\n";
        if(action!="status"){
            auto previous=guest_epoch.load(std::memory_order_acquire);
            const bool enabled=action=="start";
            while(bool(previous&1)!=enabled && !guest_epoch.compare_exchange_weak(previous,previous+1,std::memory_order_acq_rel)){}
        }
        const auto epoch=guest_epoch.load(std::memory_order_acquire);
        return "guest="+std::to_string(epoch&1)+"\nguest_epoch="+std::to_string(epoch)+
            "\nguest_coverage=android_condition_svc_1c_1d,condition_selected_notify;not_pure_guest_frames\n";
    }
    if(args[0]=="profiler_hle" && args.size()<=2){
        if(action=="start")__atomic_store_n(&mng_profile_hle_active,1,__ATOMIC_RELEASE);
        else if(action=="stop")__atomic_store_n(&mng_profile_hle_active,0,__ATOMIC_RELEASE);
        else if(action!="status")return "error=invalid_hle_action\n";
        return "hle="+std::to_string(__atomic_load_n(&mng_profile_hle_active,__ATOMIC_ACQUIRE))+"\n";
    }
    if(args[0]=="profiler_gpu"){
        if(active_backend==Backend::Metal)return "error=metal_gpu_bridge_unavailable\n";
        if(action=="start" && args.size()<=4){uint32_t budget=256,stride=16;
            if((args.size()>2&&!number(args[2],budget,1024))||(args.size()>3&&!number(args[3],stride,1024))||!budget||!stride)return "error=invalid_gpu_bounds\n";
            fine_gpu.store((uint64_t(stride)<<32)|budget);
        }else if(action=="stop" && args.size()<=2)fine_gpu.store(0);
        else if(action!="status" || args.size()>2)return "error=invalid_gpu_action\n";
        const auto config=fine_gpu.load();return "gpu_detail="+std::to_string(config!=0)+"\ngpu_budget="+std::to_string(uint32_t(config))+"\ngpu_draw_stride="+std::to_string(config>>32)+"\n";
    }
    if(args[0]=="profiler_detail"){
        if(action=="start" && args.size()<=3){uint32_t stride=1;
            if(args.size()==3 && (!number(args[2],stride,1024)||!stride))return "error=invalid_detail_stride\n";
            detail_stride.store(stride,std::memory_order_relaxed);
        }else if(action=="stop" && args.size()<=2)detail_stride.store(0,std::memory_order_relaxed);
        else if(action!="status" || args.size()>2)return "error=invalid_detail_action\n";
        const auto stride=detail_stride.load(std::memory_order_relaxed);
        return "detail="+std::to_string(stride!=0)+"\ncpu_draw_stride="+std::to_string(stride)+
            "\ncpu_detail_population=every_Nth_draw;non_draw_scopes_full\n";
    }
    if(args[0]=="profiler_ring"){
        if(args.size()<=2){
            if(action=="status")return spatial::ProfilerRing::Status();
            if(action=="start")return spatial::ProfilerRing::SetEnabled(true);
            if(action=="stop")return spatial::ProfilerRing::SetEnabled(false);
        }
        if(action=="dump" && args.size()<=3){uint32_t frames=120;if(args.size()==3&&!number(args[2],frames,100000))return "error=invalid_frames\n";return spatial::ProfilerRing::RequestDump(frames);}
    }
    if(args[0]=="profiler_capture"){
        if(args.size()<=2){
            if(action=="status")return spatial::ProfilerRing::CaptureStatus();
            if(action=="stop")return spatial::ProfilerRing::StopCapture();
        }
        if((action=="file"||action=="socket") && args.size()<=4){
            uint32_t mib=64,seconds=300;
            if((args.size()>2&&!number(args[2],mib,1024))||(args.size()>3&&!number(args[3],seconds,3600))||!mib||!seconds)return "error=invalid_capture_bounds\n";
            return action=="file"?spatial::ProfilerRing::StartFileCapture(mib,seconds):spatial::ProfilerRing::StartSocketCapture(mib,seconds);
        }
    }
    return "error=unknown_profiler_command\n";
}
void configureGpu(VkInstance instance,VkPhysicalDevice physical,VkDevice device,VkQueue queue,uint32_t family,bool calibrated){
    gpu_config.instance=instance;gpu_config.physicalDevice=physical;gpu_config.device=device;gpu_config.queue=queue;
    gpu_config.queueFamilyIndex=family;
    // The current Android Turnip calibration extension and query results have
    // not passed clock-domain validation. Use Foundation's fenced timestamp
    // midpoint calibration; it runs only when a recording context is created.
    // Keep CPU/GPU alignment unknown until the retained context is validated.
    (void)calibrated;gpu_config.calibratedTimestamps=false;
    gpu_config.getInstanceProcAddr=reinterpret_cast<void(*)()>(vkGetInstanceProcAddr);
    gpu_config.getDeviceProcAddr=reinterpret_cast<void(*)()>(vkGetDeviceProcAddr);
}
void beginGpu(GpuZone& zone,VkCommandBuffer command,const char* name){
    if(!ready.load(std::memory_order_relaxed)||!gpu_config.device||!spatial::ProfilerRing::Enabled())return;
    if(!gpu){gpu=std::make_unique<spatial::modules::RingVulkanGpuBridge>();gpu->create(&gpu_config);cacheGpuStatus();}
    zone.commandBuffer=command;
    auto [it,added]=methods.try_emplace(name);if(added)it->second.methodName=name;
    gpu->beginSample(it->second,&zone,name);
}
void endGpu(GpuZone& zone){if(gpu&&zone.active())gpu->endSample(&zone);}
void collectGpu(VkCommandBuffer command){if(gpu){GpuZone zone(command);gpu->collect(&zone);}}
void shutdownGpu(){if(gpu){gpu->destroy();gpu.reset();}methods.clear();gpu_config={};}
GpuCommandScope::GpuCommandScope(VkCommandBuffer command,const char* name,bool draw){
    const auto config=fine_gpu.load(std::memory_order_relaxed);if(!config||!spatial::ProfilerRing::Enabled())return;
    ++fine_seen;
    if(draw && draw_seen++%(config>>32)){++fine_sampled_out;return;}
    if(fine_selected>=uint32_t(config)){++fine_rejected;return;}
    beginGpu(zone,command,name);if(zone.active())++fine_selected;else ++fine_rejected;
}
void preparePresent(){
    spatial::LiteTrace::counter("GPU.Detail.Observed",fine_seen);
    spatial::LiteTrace::counter("GPU.Detail.Selected",fine_selected);
    spatial::LiteTrace::counter("GPU.Detail.SampledOut",fine_sampled_out);
    spatial::LiteTrace::counter("GPU.Detail.Rejected",fine_rejected);
    const auto config=fine_gpu.load();
    spatial::LiteTrace::counter("GPU.Detail.Budget",uint32_t(config));
    spatial::LiteTrace::counter("GPU.Detail.DrawStride",config>>32);
    spatial::LiteTrace::counter("CPU.Detail.Enabled",detail_stride.load()!=0);
    spatial::LiteTrace::counter("CPU.Detail.DrawStride",detail_stride.load());
    spatial::LiteTrace::counter("CPU.HLE.Enabled",__atomic_load_n(&mng_profile_hle_active,__ATOMIC_ACQUIRE));
    fine_seen=fine_selected=fine_rejected=fine_sampled_out=draw_seen=0;
    if(++presents%30==0)cacheGpuStatus();
}
void present(uint64_t token,bool prepared){
    if(!prepared)preparePresent();
    spatial::ProfilerRing::FrameMark();
    spatial::LiteTrace::counter("Native.Present.Token",int64_t(token));
}
}
