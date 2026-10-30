// SPDX-License-Identifier: MIT
#include "../runtime/lane_emit.h"
#include "spatial/core/utils/ProfilerRing.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <time.h>

namespace {
uint64_t clock_ns(clockid_t clock) {
    timespec t{};assert(clock_gettime(clock,&t)==0);
    return uint64_t(t.tv_sec)*1000000000ull+t.tv_nsec;
}
void emit(unsigned frame) {
    static mng_lane_accounting lanes=[] {
        mng_lane_accounting s{};mng_lane_reset(&s,1);
        mng_lane_find(&s,1,0x8001);s.rows[0].tid=1;return s;
    }();
    uint64_t start=clock_ns(CLOCK_MONOTONIC);
    lanes.previous=start;
    auto packet=mng_lane_packet_begin(&lanes,frame,start+1400,1);
    for(unsigned i=0;i<1400;++i){
        mng_lane_event event{start+i,0,MNG_LANE_OWNER,i%2?-1:0,0,0,0};
        mng_lane_packet_event(&packet,&event);
    }
    mng_lane_packet_end(&packet);
    spatial::ProfilerRing::FrameMark();
}
}
int main(int argc,char** argv) {
    if(argc!=3 || (std::string(argv[2])!="off" && std::string(argv[2])!="ring" && std::string(argv[2])!="file")){
        fprintf(stderr,"Usage: profile-cost-probe OUTPUT_DIRECTORY off|ring|file\n");return 2;
    }
    std::filesystem::create_directories(argv[1]);
    using P=spatial::ProfilerRing;
    P::Initialize("mng-profile-cost",argv[1],false,16*1024*1024);
    P::SetAppInfo("synthetic bounded profiler emission microbenchmark; not game performance");
    const std::string mode=argv[2];
    if(mode=="ring")P::SetEnabled(true);
    if(mode=="file"){
        const auto result=P::StartFileCapture(64,30);
        assert(result.find("capture_active=1")!=std::string::npos);
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(P::CaptureStatus().find("capture_state=recording")==std::string::npos){
            assert(std::chrono::steady_clock::now()<deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    // The first synthetic workload frame applies the SDK route transition.
    for(unsigned i=1;i<=20;++i)emit(i);
    assert(P::Enabled()==(mode!="off"));
    for(unsigned round=0;round<4;++round){
        auto wall=clock_ns(CLOCK_MONOTONIC),cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID);
        for(unsigned i=0;i<120;++i)emit(21+round*120+i);
        cpu=clock_ns(CLOCK_THREAD_CPUTIME_ID)-cpu;wall=clock_ns(CLOCK_MONOTONIC)-wall;
        printf("{\"mode\":\"%s\",\"round\":%u,\"frames\":120,\"events_per_frame\":1400,\"wall_ns\":%llu,\"thread_cpu_ns\":%llu}\n",
               mode.c_str(),round,(unsigned long long)wall,(unsigned long long)cpu);
        fflush(stdout);
    }
    if(mode=="file"){
        P::StopCapture();
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        while(P::CaptureStatus().find("capture_active=1")!=std::string::npos){
            if(std::chrono::steady_clock::now()>deadline)return 3;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto status=P::CaptureStatus();
        if(status.find("capture_state=ready")==std::string::npos){fprintf(stderr,"%s",status.c_str());return 4;}
    }
    P::SetEnabled(false);
}
