// SPDX-License-Identifier: MIT
#include "../porting/profiler/profiler.h"
#include "../porting/profiler/debugbus.h"
#include "../porting/profiler/socket_debugbus.h"
#include "../porting/runtime/port_runtime.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <mutex>
#include <unistd.h>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <thread>
#include "../porting/runtime/host_thread.h"
namespace {std::unique_ptr<mng::diagnostics::SocketDebugBus> debugbus;}
extern "C" void mng_macos_vulkan_ready();
extern "C" int runtime_profile_threads(char*,unsigned);
extern "C" int runtime_lane_control(int,char*,unsigned);
extern "C" void runtime_lane_present(uint64_t);
extern "C" int runtime_autoload_snapshot(char*,unsigned);
extern "C" int runtime_system_mode_snapshot(char*,unsigned);
extern "C" int runtime_input_pulse(int,unsigned,char*,unsigned);
extern "C" void mng_macos_initialize(const char* backend) {
    const char* directory=getenv("MNG_PROFILE_DIR");if(!directory)directory=getenv("TOTK_PROFILE_DIR");
    if(!directory){fprintf(stderr,"MNG_PROFILE_DIR is required for the shared macOS host\n");exit(2);}
    setenv("MNG_PORT_DEBUG","1",0);
    setenv("TOTK_OUTPUT_RES","0",0);
    if(!getenv("TOTK_OPERATION_MODE") && !getenv("MNG_RENDER_RESOLUTION"))
        setenv("TOTK_OPERATION_MODE","handheld",0);
    mng::profile::initialize("MacNativeGames:macOS",directory,std::string("title=BOTW version=1.6.0 platform=macos-arm64 backend=")+backend,!strcmp(backend,"metal")?mng::profile::Backend::Metal:mng::profile::Backend::Vulkan);
    mng::diagnostics::setThreadSnapshot(runtime_profile_threads);
    mng::diagnostics::setLaneCallbacks(runtime_lane_control,runtime_lane_present);
    mng::diagnostics::setAutoloadSnapshot(runtime_autoload_snapshot);
    mng::diagnostics::setSystemModeSnapshot(runtime_system_mode_snapshot);
    mng::diagnostics::setInputPulse(runtime_input_pulse);
    const char* socket=getenv("MNG_DEBUGBUS_SOCKET");
    const std::string path=socket?socket:"/tmp/mng-botw-"+std::to_string(getpid())+".sock";
    debugbus=std::make_unique<mng::diagnostics::SocketDebugBus>(path);
    fprintf(stderr,"[mng-debugbus] pid=%d socket=%s backend=%s\n",getpid(),path.c_str(),backend);
#ifdef MNG_BACKEND_VULKAN
    mng_macos_vulkan_ready();
#endif
}
extern "C" void mng_macos_modules_ready() {
    mng::diagnostics::setRuntime(mng::port::debug_snapshot());
}
namespace {
// FrameMark must have a stable physical thread owner. Drawable callbacks only
// enqueue completed game presents; generated frames are filtered at the source.
struct MetalFrames {
    struct Event {uint64_t ticket;std::chrono::steady_clock::time_point at;};
    std::mutex mutex;std::condition_variable cv;std::deque<Event> pending;
    bool stopping{};uint64_t dropped{};std::thread worker{[this]{run();}};
    ~MetalFrames(){{std::lock_guard lock(mutex);stopping=true;}cv.notify_one();worker.join();}
    void push(uint64_t ticket){std::lock_guard lock(mutex);
        if(pending.size()==64){++dropped;return;}
        pending.push_back({ticket,std::chrono::steady_clock::now()});cv.notify_one();}
    void run(){mng_name_thread("mng-metal-frames");
        std::chrono::steady_clock::time_point previous{};
        uint64_t reported=0;
        for(;;){Event event;uint64_t lost;
            {std::unique_lock lock(mutex);cv.wait(lock,[&]{return stopping||!pending.empty();});
             if(pending.empty())return;event=pending.front();pending.pop_front();lost=dropped;}
            mng::diagnostics::FrameStats sample;sample.frame=event.ticket;
            sample.present_count=++reported;
            if(previous.time_since_epoch().count())sample.frame_ms=std::chrono::duration<double,std::milli>(event.at-previous).count();
            previous=event.at;
            spatial::LiteTrace::counter("Native.Present.DroppedReports",int64_t(lost));
            mng::profile::present(event.ticket);mng::diagnostics::frame(sample);
        }
    }
};
}
extern "C" void mng_macos_metal_present(uint64_t ticket){static MetalFrames frames;frames.push(ticket);}
