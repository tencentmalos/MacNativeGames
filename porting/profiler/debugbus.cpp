// SPDX-License-Identifier: MIT
#include "debugbus.h"
#include "profiler.h"
#include "performance_hint.h"
#include "spatial/debugbus/DebugCommandRegistry.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <unistd.h>

namespace mng::diagnostics {
namespace {
using Clock=std::chrono::steady_clock;
std::mutex cache_mutex,request_mutex;
std::string identity="identity=not_initialized\n",runtime="{\"phase\":\"not_initialized\"}";
FrameStats latest;
Clock::time_point updated;
std::atomic<ThreadSnapshot> thread_snapshot{},resource_snapshot{},target_snapshot{},system_mode_snapshot{};
std::atomic<ScheduleControl> schedule_control{};
std::atomic<FlushImageControl> flush_image_control{},frontlock_control{},ordered_inputs_control{},publication_control{},invalidate_control{};
std::atomic<InputPulse> input_pulse{};
std::atomic<FlushImageControl> lane_control{};
std::atomic<void (*)(uint64_t)> lane_present{};
std::atomic<ThreadSnapshot> flush_snapshot{};
std::atomic<ThreadSnapshot> autoload_snapshot{};
const char* busy="error=emulation_session_busy\nretryable=1\n";
std::string status(bool render) {
    FrameStats stats;std::string id;Clock::time_point time;
    {std::unique_lock lock(cache_mutex,std::try_to_lock);if(!lock.owns_lock())return busy;
     stats=latest;id=identity;time=updated;}
    std::ostringstream out;out<<"schema=mng.debugbus.v1\nprocess_id="<<getpid()<<"\n"<<id;
    const bool metal=id.find("backend=metal")!=std::string::npos;
    out<<(metal?"frame_source=metal_drawable_presented\nguest_frame_identity=present_ticket\n":"frame_source=successful_native_present\nguest_frame_identity=unknown\n")<<"frame="<<stats.frame<<"\n";
    if(!stats.frame){out<<"render_state=no_present_yet\n";return out.str();}
    if(metal)out<<"present_count="<<stats.present_count<<"\nframe_counter=presentation_ticket;may_skip_coalesced_frames\n";
    out<<"sample_age_ms="<<std::chrono::duration<double,std::milli>(Clock::now()-time).count()<<"\n";
    if(render && metal){out<<"frame_ms="<<stats.frame_ms<<"\nrender_counters_available=0\nreason=metal_counter_adapter_unavailable\n";return out.str();}
    if(render)out<<"frame_ms="<<stats.frame_ms<<"\ncpu_process_ms="<<stats.cpu_process_ms
        <<"\npresent_interval="<<stats.present_interval<<"\npresent_ms="<<stats.present_ms<<"\ngpu_ms="<<stats.gpu_ms<<"\ngpu_frame="<<stats.gpu_frame
        <<"\ndraws="<<stats.draws<<"\ndispatches="<<stats.dispatches<<"\npipelines_created="<<stats.pipelines
        <<"\nshaders_loaded="<<stats.shaders<<"\nupload_bytes="<<stats.upload_bytes
        <<"\npipeline_ms="<<stats.pipeline_ms<<"\nshader_ms="<<stats.shader_ms<<"\nupload_ms="<<stats.upload_ms<<"\n";
    return out.str();
}
spatial::debugbus::DebugCommandRegistry makeRegistry() {
    spatial::debugbus::DebugCommandRegistry registry;
    for(auto name:{"profiler_ring","profiler_capture","profiler_detail","profiler_hle","profiler_guest","profiler_tags","profiler_address","profiler_gpu","profiler_ring_gpu","profiler_coverage"})
        registry.Register(name,"Litep control; profiler_coverage describes modes and limits",[name](const auto& args){
            std::string command=name;for(const auto& arg:args)command+=" "+arg;return profile::command(command);});
    registry.Register("debug_status","Process identity and last successful present",[](const auto& args){return args.empty()?status(false):"error=unexpected_arguments\n";});
    registry.Register("autoload_status","Bounded save-load automation state",[](const auto& args)->std::string{
        if(!args.empty())return "error=unexpected_arguments\n";
        auto callback=autoload_snapshot.load(std::memory_order_acquire);if(!callback)return "error=autoload_unavailable\nretryable=1\n";
        char text[512]{};return callback(text,sizeof text)<0?busy:std::string(text);
    });
    registry.Register("renderer_status","Cached render counters; never waits for GPU",[](const auto& args){return args.empty()?status(true):"error=unexpected_arguments\n";});
    registry.Register("renderer_hint","Host work-duration hints: status/on/off (default off)",[](const auto& args)->std::string{
        int enabled=-1;
        if(args.size()==1 && (args[0]=="on" || args[0]=="off"))enabled=args[0]=="on";
        else if(!args.empty() && !(args.size()==1 && args[0]=="status"))return "error=invalid_hint_action\n";
        return mng::port::performance_hint_control(enabled);
    });
    registry.Register("renderer_flush","Diagnostic Flush ranges against current images and writebacks",[](const auto& args)->std::string{
        if(!args.empty())return "error=unexpected_arguments\n";
        auto callback=flush_snapshot.load(std::memory_order_acquire);if(!callback)return "error=renderer_unavailable\n";
        char text[16384]{};return callback(text,sizeof text)<0?busy:std::string(text);
    });
    registry.Register("input_pulse","Bounded movement: status | stop | forward/backward/left/right/look_left/look_right 1..3000 ms",[](const auto& args)->std::string{
        int direction=0;unsigned ms=0;
        if(args.size()==1 && args[0]=="status"){}
        else if(args.size()==1 && args[0]=="stop")ms=1;
        else if(args.size()==2 && !args[1].empty() && args[1].size()<=4){
            const char* names[]={"forward","backward","left","right","look_left","look_right"};
            for(int i=0;i<6;++i)if(args[0]==names[i])direction=i+1;
            for(char c:args[1]){if(c<'0'||c>'9')return "error=invalid_input_pulse\n";ms=ms*10+c-'0';}
            if(!direction || !ms || ms>3000)return "error=invalid_input_pulse\n";
        }else return "error=invalid_input_pulse\n";
        auto callback=input_pulse.load(std::memory_order_acquire);if(!callback)return "error=input_unavailable\n";
        char text[512]{};return callback(direction,ms,text,sizeof text)<0?busy:std::string(text);
    });
    registry.Register("renderer_schedule","Scheduling A/B: status | adaptive/present/invalidate/reads/descriptors/images/pipeline/plans/flush on/off | batch 0..65536",[](const auto& args)->std::string{
        int adaptive=-1,batch=-1,present=-1,invalidate=-1,reads=-1,descriptors=-1,images=-1,pipeline=-1,plans=-1,flush=-1;
        if(args.empty() || (args.size()==1 && args[0]=="status")){}
        else if(args.size()==2 && args[0]=="adaptive" && (args[1]=="on" || args[1]=="off"))adaptive=args[1]=="on";
        else if(args.size()==2 && args[0]=="present" && (args[1]=="on" || args[1]=="off"))present=args[1]=="on";
        else if(args.size()==2 && args[0]=="invalidate" && (args[1]=="on" || args[1]=="off"))invalidate=args[1]=="on";
        else if(args.size()==2 && args[0]=="reads" && (args[1]=="on" || args[1]=="off"))reads=args[1]=="on";
        else if(args.size()==2 && args[0]=="descriptors" && (args[1]=="on" || args[1]=="off"))descriptors=args[1]=="on";
        else if(args.size()==2 && args[0]=="flush" && (args[1]=="on" || args[1]=="off"))flush=args[1]=="on";
        else if(args.size()==2 && args[0]=="plans" && (args[1]=="on" || args[1]=="off"))plans=args[1]=="on";
        else if(args.size()==2 && args[0]=="pipeline" && (args[1]=="on" || args[1]=="off"))pipeline=args[1]=="on";
        else if(args.size()==2 && args[0]=="images" && (args[1]=="on" || args[1]=="off"))images=args[1]=="on";
        else if(args.size()==2 && args[0]=="batch" && !args[1].empty() && args[1].size()<=5){
            batch=0;for(char c:args[1]){if(c<'0' || c>'9')return "error=invalid_schedule\n";batch=batch*10+c-'0';}
            if(batch>65536)return "error=invalid_schedule\n";
        }else return "error=invalid_schedule\n";
        auto callback=schedule_control.load(std::memory_order_acquire);if(!callback)return "error=renderer_schedule_unavailable\n";
        char text[4096]{};return callback(adaptive,batch,present,invalidate,reads,descriptors,images,pipeline,plans,flush,text,sizeof text)<0?busy:std::string(text);
    });
    registry.Register("renderer_flush_image","Shared coherent image Flush publication: status/on/off",[](const auto& args)->std::string{
        int enabled=-1;
        if(args.size()==1 && (args[0]=="on" || args[0]=="off"))enabled=args[0]=="on";
        else if(!args.empty() && !(args.size()==1 && args[0]=="status"))return "error=invalid_flush_image_action\n";
        auto callback=flush_image_control.load(std::memory_order_acquire);if(!callback)return "error=flush_image_unavailable\n";
        char out[512]{};return callback(enabled,out,sizeof out)<0?busy:std::string(out);
    });
    registry.Register("lane_status","Virtual-core accounting: on/off/status/reset (default off)",[](const auto& args)->std::string{
        int action=-1;
        if(args.size()==1 && args[0]=="on")action=1;
        else if(args.size()==1 && args[0]=="off")action=0;
        else if(args.size()==1 && args[0]=="reset")action=2;
        else if(!args.empty() && !(args.size()==1 && args[0]=="status"))return "error=invalid_lane_action\n";
        auto callback=lane_control.load(std::memory_order_acquire);if(!callback)return "error=lane_unavailable\n";
        std::string out(262144,'\0');int n=callback(action,out.data(),out.size());
        if(n<0)return busy;out.resize(n);return out;
    });
    registry.Register("renderer_invalidate","Shared Vulkan range Invalidate: status/on/off",[](const auto& args)->std::string{
        int enabled=-1;
        if(args.size()==1 && (args[0]=="on" || args[0]=="off"))enabled=args[0]=="on";
        else if(!args.empty() && !(args.size()==1 && args[0]=="status"))return "error=invalid_invalidate_action\n";
        auto callback=invalidate_control.load(std::memory_order_acquire);if(!callback)return "error=invalidate_unavailable\n";
        char out[1024]{};return callback(enabled,out,sizeof out)<0?busy:std::string(out);
    });
    registry.Register("renderer_publication","Shared Vulkan publication queue: status/on/off",[](const auto& args)->std::string{
        int enabled=-1;
        if(args.size()==1 && (args[0]=="on" || args[0]=="off"))enabled=args[0]=="on";
        else if(!args.empty() && !(args.size()==1 && args[0]=="status"))return "error=invalid_publication_action\n";
        auto callback=publication_control.load(std::memory_order_acquire);if(!callback)return "error=publication_unavailable\n";
        char out[2048]{};return callback(enabled,out,sizeof out)<0?busy:std::string(out);
    });
    registry.Register("renderer_ordered_inputs","Ordered input rejection diagnostics: status",[](const auto& args)->std::string{
        if(!args.empty() && !(args.size()==1 && args[0]=="status"))return "error=unexpected_arguments\n";
        auto callback=ordered_inputs_control.load(std::memory_order_acquire);if(!callback)return "error=ordered_inputs_unavailable\n";
        char out[2048]{};return callback(-1,out,sizeof out)<0?busy:std::string(out);
    });
    registry.Register("frontlock_status","Bounded front lock diagnostics: status/on/off/reset",[](const auto& args)->std::string{
        int action=-1;
        if(args.empty() || (args.size()==1 && args[0]=="status")){}
        else if(args.size()==1 && args[0]=="on")action=1;
        else if(args.size()==1 && args[0]=="off")action=0;
        else if(args.size()==1 && args[0]=="reset")action=2;
        else return "error=invalid_frontlock_action\n";
        auto callback=frontlock_control.load(std::memory_order_acquire);if(!callback)return "error=frontlock_unavailable\n";
        char out[65536]{};return callback(action,out,sizeof out)<0?busy:std::string(out);
    });
    registry.Register("guest_modules","Verified loader snapshot, including module Build IDs",[](const auto& args)->std::string{
        if(!args.empty())return "error=unexpected_arguments\n";
        std::unique_lock lock(cache_mutex,std::try_to_lock);if(!lock.owns_lock())return busy;return runtime+"\n";});
    registry.Register("threads","Bounded guest thread/host TID/virtual core snapshot",[](const auto& args)->std::string{
        if(!args.empty())return "error=unexpected_arguments\n";
        auto callback=thread_snapshot.load(std::memory_order_acquire);if(!callback)return "error=guest_threads_not_initialized\n";
        char text[32768]{};const int result=callback(text,sizeof text);return result<0?busy:std::string(text);});
    registry.Register("renderer_resources","Nonblocking native renderer lifetime counters",[](const auto& args)->std::string{
        if(!args.empty())return "error=unexpected_arguments\n";
        auto callback=resource_snapshot.load(std::memory_order_acquire);if(!callback)return "error=renderer_not_initialized\n";
        char text[2048]{};return callback(text,sizeof text)<0?busy:std::string(text);});
    registry.Register("renderer_targets","Last completed frame render-pass extents",[](const auto& args)->std::string{
        if(!args.empty())return "error=unexpected_arguments\n";
        auto callback=target_snapshot.load(std::memory_order_acquire);if(!callback)return "error=renderer_not_initialized\n";
        char text[32768]{};return callback(text,sizeof text)<0?busy:std::string(text);});
    registry.Register("system_mode","Immutable launch mode and observed Guest OE query counts",[](const auto& args)->std::string{
        if(!args.empty())return "error=unexpected_arguments\n";
        auto callback=system_mode_snapshot.load(std::memory_order_acquire);if(!callback)return "error=system_mode_not_initialized\n";
        char text[2048]{};return callback(text,sizeof text)<0?busy:std::string(text);});
    return registry;
}
}
void setIdentity(std::string value){std::lock_guard lock(cache_mutex);identity=std::move(value);}
void setRuntime(std::string value){if(value.size()>256*1024)value="{\"error\":\"snapshot_too_large\"}";std::lock_guard lock(cache_mutex);runtime=std::move(value);}
void frame(const FrameStats& stats){if(auto callback=lane_present.load(std::memory_order_acquire))callback(stats.frame);std::unique_lock lock(cache_mutex,std::try_to_lock);if(lock.owns_lock()){latest=stats;updated=Clock::now();}}
void setLaneCallbacks(FlushImageControl control,void (*present)(uint64_t)){lane_control.store(control,std::memory_order_release);lane_present.store(present,std::memory_order_release);}
void setThreadSnapshot(ThreadSnapshot callback){thread_snapshot.store(callback,std::memory_order_release);}
void setTargetSnapshot(ThreadSnapshot callback){target_snapshot.store(callback,std::memory_order_release);}
void setFlushSnapshot(ThreadSnapshot callback){flush_snapshot.store(callback,std::memory_order_release);}
void setSystemModeSnapshot(ThreadSnapshot callback){system_mode_snapshot.store(callback,std::memory_order_release);}
void setAutoloadSnapshot(ThreadSnapshot callback){autoload_snapshot.store(callback,std::memory_order_release);}
void setResourceSnapshot(ThreadSnapshot callback){resource_snapshot.store(callback,std::memory_order_release);}
void setFlushImageControl(FlushImageControl callback){flush_image_control.store(callback,std::memory_order_release);}
void setFrontLockControl(FlushImageControl callback){frontlock_control.store(callback,std::memory_order_release);}
void setOrderedInputsControl(FlushImageControl callback){ordered_inputs_control.store(callback,std::memory_order_release);}
void setInvalidateControl(FlushImageControl callback){invalidate_control.store(callback,std::memory_order_release);}
void setPublicationControl(FlushImageControl callback){publication_control.store(callback,std::memory_order_release);}
void setScheduleControl(ScheduleControl callback){schedule_control.store(callback,std::memory_order_release);}
void setInputPulse(InputPulse callback){input_pulse.store(callback,std::memory_order_release);}
std::string command(const std::string& request){
    if(request.size()>256)return "error=command_too_long\n";
    std::unique_lock lock(request_mutex,std::try_to_lock);if(!lock.owns_lock())return "error=debugbus_busy\nretryable=1\n";
    static const auto registry=makeRegistry();return registry.Handle(request);
}
}
