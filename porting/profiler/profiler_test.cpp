// SPDX-License-Identifier: MIT
#include "profiler.h"
#include "profile_c.h"
#include "debugbus.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <unistd.h>
using mng::diagnostics::command;
extern "C" {
// The production assembly is linked directly. Test callbacks deliberately
// exercise the independent integer/SIMD argument and result register banks.
uint64_t runtime_hle_tsd_off;
void* runtime_hle_targets[4096];
int runtime_hle_trace;
void runtime_hle_note(uint64_t){}
static unsigned begin_calls;
uint64_t runtime_hle_profile_begin(unsigned index){assert(index==17);++begin_calls;return mng_profile_begin("HLE.TestABI");}
struct IntegerPair{uint64_t a,b;};
struct FloatPair{double a,b;};
#define ARGS uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h,double i,double j,double k,double l,double m,double n,double o,double p
#define SUMI (a+2*b+3*c+4*d+5*e+6*f+7*g+8*h)
#define SUMF (i+2*j+3*k+4*l+5*m+6*n+7*o+8*p)
IntegerPair mng_test_integer_target(ARGS){return {SUMI,uint64_t(SUMF)};}
FloatPair mng_test_float_target(ARGS){return {double(SUMI),SUMF};}
IntegerPair mng_test_integer(ARGS);
FloatPair mng_test_float(ARGS);
}
int main(){
    assert(command("help").find("guest_modules")!=std::string::npos);
    assert(command(std::string(257,'x')).find("command_too_long")!=std::string::npos);
    assert(command("profiler_ring status").find("not_initialized")!=std::string::npos);
    assert(command("renderer_status").find("no_present_yet")!=std::string::npos);
    assert(command("renderer_schedule status").find("unavailable")!=std::string::npos);
    mng::diagnostics::setScheduleControl([](int adaptive,int batch,int,int,int reads,int descriptors,int images,int pipeline,int plans,int flush,char* out,unsigned bytes){
        return snprintf(out,bytes,"adaptive=%d\nbatch=%d\nreads=%d\ndescriptors=%d\nimages=%d\npipeline=%d\nplans=%d\n",adaptive,batch,reads,descriptors,images,pipeline,plans);
    });
    assert(command("renderer_schedule adaptive off").find("adaptive=0")!=std::string::npos);
    assert(command("renderer_schedule batch 65536").find("batch=65536")!=std::string::npos);
    assert(command("renderer_schedule reads off").find("reads=0")!=std::string::npos);
    assert(command("renderer_schedule descriptors off").find("descriptors=0")!=std::string::npos);
    assert(command("renderer_schedule images on").find("images=1")!=std::string::npos);
    assert(command("renderer_schedule pipeline off").find("pipeline=0")!=std::string::npos);
    assert(command("renderer_schedule plans on").find("plans=1")!=std::string::npos);
    assert(command("renderer_schedule plans off").find("plans=0")!=std::string::npos);
    for(auto request:{"plans maybe","pipeline maybe","images maybe","descriptors invalid","reads invalid","batch 65537","batch -1","batch 1x","adaptive maybe","status extra"})
        assert(command(std::string("renderer_schedule ")+request).find("invalid_schedule")!=std::string::npos);
    mng::diagnostics::setScheduleControl([](int,int,int,int,int,int,int,int,int,int,char*,unsigned){return -1;});
    assert(command("renderer_schedule status").find("retryable=1")!=std::string::npos);
    mng::diagnostics::setScheduleControl(nullptr);
    assert(command("input_pulse status").find("unavailable")!=std::string::npos);
    mng::diagnostics::setInputPulse([](int direction,unsigned ms,char* out,unsigned bytes){
        return snprintf(out,bytes,"direction=%d\nmilliseconds=%u\n",direction,ms);
    });
    assert(command("input_pulse forward 1200").find("milliseconds=1200")!=std::string::npos);
    assert(command("input_pulse look_right 3000").find("direction=6")!=std::string::npos);
    assert(command("input_pulse stop").find("milliseconds=1")!=std::string::npos);
    for(auto request:{"forward 0","forward 3001","forward -1","forward 999999","unknown 10","status extra"})
        assert(command(std::string("input_pulse ")+request).find("invalid_input_pulse")!=std::string::npos);
    mng::diagnostics::setInputPulse([](int,unsigned,char*,unsigned){return -1;});
    assert(command("input_pulse forward 1").find("retryable=1")!=std::string::npos);
    mng::diagnostics::setInputPulse(nullptr);
    mng::profile::initialize("mng-profiler-test","/data/local/tmp/mng-profiler-test","title=test");
    assert(command("profiler_guest status").find("guest=0\n")!=std::string::npos);
    assert(command("profiler_guest start").find("guest_epoch=1\n")!=std::string::npos);
    assert(command("profiler_guest start").find("guest_epoch=1\n")!=std::string::npos);
    assert(command("profiler_guest stop").find("guest_epoch=2\n")!=std::string::npos);
    assert(command("profiler_guest start").find("guest_epoch=3\n")!=std::string::npos);
    assert(command("profiler_guest start extra").find("invalid_guest_action")!=std::string::npos);
    assert(command("profiler_guest unknown").find("invalid_guest_action")!=std::string::npos);
    assert(command("profiler_guest stop").find("guest=0\n")!=std::string::npos);
    assert(command("profiler_detail start 0").find("invalid_detail_stride")!=std::string::npos);
    assert(command("profiler_detail start 1025").find("invalid_detail_stride")!=std::string::npos);
    assert(command("profiler_detail start 8").find("cpu_draw_stride=8")!=std::string::npos);
    unsigned selected=0;
    for(unsigned i=0;i<80;++i){mng::profile::CpuDetailDrawScope sample;
        const bool outer=mng::profile::detailedCpu();selected+=outer;
        {mng::profile::CpuDetailDrawScope nested;assert(mng::profile::detailedCpu()==outer);}
        assert(mng::profile::detailedCpu()==outer);
    }
    assert(selected==10 && mng::profile::detailedCpu());
    command("profiler_detail stop");assert(!mng::profile::detailedCpu());
    assert(command("profiler_gpu start 0 16").find("invalid_gpu_bounds")!=std::string::npos);
    assert(command("profiler_gpu start 1025 16").find("invalid_gpu_bounds")!=std::string::npos);
    assert(command("profiler_gpu start 256 16").find("gpu_budget=256")!=std::string::npos);
    assert(command("profiler_gpu stop").find("gpu_detail=0")!=std::string::npos);
    mng::diagnostics::setThreadSnapshot([](char*,unsigned){return -1;});
    assert(command("threads").find("retryable=1")!=std::string::npos);
    mng::diagnostics::setThreadSnapshot(nullptr);
    mng::diagnostics::setResourceSnapshot([](char*,unsigned){return -1;});
    assert(command("renderer_resources").find("retryable=1")!=std::string::npos);
    mng::diagnostics::setResourceSnapshot([](char *out,unsigned bytes){return snprintf(out,bytes,"host_objects=42\n");});
    assert(command("renderer_resources").find("host_objects=42")!=std::string::npos);
    assert(command("renderer_resources extra").find("unexpected_arguments")!=std::string::npos);
    mng::diagnostics::setResourceSnapshot(nullptr);
    assert(command("system_mode").find("not_initialized")!=std::string::npos);
    mng::diagnostics::setSystemModeSnapshot([](char *out,unsigned bytes){return snprintf(out,bytes,"mode=handheld\noperation_queries=1\n");});
    assert(command("system_mode").find("operation_queries=1")!=std::string::npos);
    assert(command("system_mode extra").find("unexpected_arguments")!=std::string::npos);
    mng::diagnostics::setSystemModeSnapshot(nullptr);
    for(bool enabled:{false,true}){
        command(enabled?"profiler_ring start":"profiler_ring stop");
        for(unsigned mode:{0u,1u}){
            command(mode?"profiler_hle start":"profiler_hle stop");auto before=begin_calls;
            for(int i=0;i<128;++i){
                auto a=mng_test_integer(1,2,3,4,5,6,7,8,1.,2.,3.,4.,5.,6.,7.,8.);
                auto b=mng_test_float(1,2,3,4,5,6,7,8,1.,2.,3.,4.,5.,6.,7.,8.);
                assert(a.a==204 && a.b==204 && b.a==204. && b.b==204.);
            }
            assert(begin_calls-before==(mode?256:0));
        }
    }
    command("profiler_ring stop");command("profiler_hle stop");
    std::thread owner([]{for(unsigned i=1;i<=5000;++i){mng::diagnostics::FrameStats stats;stats.frame=i;mng::diagnostics::frame(stats);}});
    for(unsigned i=0;i<1000;++i){auto reply=command("renderer_status");assert(reply.find("schema=")!=std::string::npos||reply.find("retryable=1")!=std::string::npos);}
    owner.join();
    mng::diagnostics::FrameStats stats;stats.frame=5001;stats.present_interval=2;
    mng::diagnostics::frame(stats);
    assert(command("renderer_status").find("present_interval=2")!=std::string::npos);
    puts("PASS: debugbus bounds/busy/cache; real HLE assembly integer/SIMD ABI, enabled/disabled recording");
}
