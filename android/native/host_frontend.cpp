// SPDX-License-Identifier: MIT
// VI adapter; the process probe explicitly uses a headless display.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <sys/system_properties.h>
#include <android/native_window.h>
#include "../../porting/runtime/display_tick.h"
#include "profiler.h"
extern "C" {
[[noreturn]] void runtime_fail(const char*);
void hle_bind_system_event(void*,int);
void hle_signal_system_event(void*);
}
namespace {
unsigned display_token,layer_token;bool ready;
std::atomic<ANativeWindow*> native_window;std::atomic<bool> focused;
std::mutex mutex;void* events[16];unsigned count;
std::mutex display_mutex;std::condition_variable display_cv;
mng::port::DisplayTickClock display_clock;
void initialize() {
    const auto* mode=getenv("MNG_HEADLESS");
    if(!native_window.load() && (!mode||strcmp(mode,"1")))runtime_fail("Native VI requires an Android Surface; standalone rendering requires explicit MNG_HEADLESS=1");
    ready=true;fprintf(stderr,"[android-boot] VI display: %s\n",native_window.load()?"Android Surface":"explicit headless probe");
}
uint32_t open(void** out){if(!ready||!out)return 0xf601;*out=&display_token;return 0;}
uint32_t layer(void** out,void* display){if(!out||display!=&display_token)return 0xf601;*out=&layer_token;return 0;}
uint32_t window(void** out,void* layer){if(!out||layer!=&layer_token)return 0xf601;*out=&layer_token;return 0;}
uint32_t vsync(void* event,void* display) {
    if(!event||display!=&display_token)return 0xf601;
    hle_bind_system_event(event,1);
    std::lock_guard lock(mutex);if(count==16)runtime_fail("VI event limit");
    events[count++]=event;
    static bool running=false;
    if(!running && !native_window.load()){running=true;std::thread([]{
        auto next=std::chrono::steady_clock::now();
        for(;;){next+=std::chrono::nanoseconds(16666667);std::this_thread::sleep_until(next);
            std::lock_guard guard(mutex);for(unsigned i=0;i<count;++i)hle_signal_system_event(events[i]);}
    }).detach();}
    return 0;
}
uint32_t scaling(void* layer,int mode){if(layer!=&layer_token||mode<0||mode>2)return 0xf601;return 0;}
}
extern "C" void* runtime_front_override(const char* name) {
    struct Entry{const char* name;void* fn;};
    const Entry entries[]={
        {"_ZN2nn2vi10InitializeEv",reinterpret_cast<void*>(initialize)},
        {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE",reinterpret_cast<void*>(open)},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE",reinterpret_cast<void*>(layer)},
        {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE",reinterpret_cast<void*>(window)},
        {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE",reinterpret_cast<void*>(vsync)},
        {"_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE",reinterpret_cast<void*>(scaling)}};
    for(const auto& e:entries)if(!strcmp(name,e.name))return e.fn;return nullptr;
}

extern "C" void mng_android_window(ANativeWindow *window) {native_window.store(window);}
extern "C" ANativeWindow *mng_android_get_window() {return native_window.load();}
extern "C" void mng_android_focus(bool value) {focused.store(value);display_cv.notify_all();}
extern "C" int runtime_front_focus() {return focused.load()?1:0;}
extern "C" void mng_android_vsync(uint64_t time,float hz) {
    {std::lock_guard lock(display_mutex);display_clock.tick(time,hz);}
    display_cv.notify_all();
    std::lock_guard lock(mutex);for(unsigned i=0;i<count;++i)hle_signal_system_event(events[i]);
}
extern "C" int mng_android_wait_display_tick(uint64_t* previous,int interval) {
    static const bool enabled=[] {char p[PROP_VALUE_MAX]{};__system_property_get("debug.mng.display_pace",p);return !strcmp(p,"1");}();
    if(!enabled || !previous || interval<=0 || !focused.load())return 0;
    const auto now=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    std::unique_lock lock(display_mutex);uint64_t step{};
    if(!display_clock.step(now,unsigned(interval),step)){*previous=0;return 0;}
    const auto target=*previous?*previous+step:display_clock.sequence();
    if(display_clock.sequence()<target) {
        spatial::LiteTraceScope scope("Host.WindowAcquire.DisplayTick");
        if(!display_cv.wait_for(lock,std::chrono::nanoseconds(uint64_t(interval)*2000000000ull/60),[&]{return !focused.load()||display_clock.sequence()>=target;})) {
            *previous=0;return 0;
        }
    }
    if(!focused.load()){*previous=0;return 0;}
    *previous=display_clock.sequence();return 1;
}
