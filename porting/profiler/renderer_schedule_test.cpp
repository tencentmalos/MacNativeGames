// SPDX-License-Identifier: MIT
#include "debugbus.h"
#include "profiler.h"
#include "profile_c.h"
#include "../runtime/lane_emit.h"
#include <filesystem>
#include <chrono>
#include <array>
#include <cassert>
#include <cstdio>
#include <string>
using mng::diagnostics::command;
namespace {
std::array<int,10> received;
int schedule(int adaptive,int batch,int present,int invalidate,int reads,int descriptors,
             int images,int pipeline,int plans,int flush,char *out,unsigned bytes){
    received={adaptive,batch,present,invalidate,reads,descriptors,images,pipeline,plans,flush};
    return snprintf(out,bytes,"preparation_plan=%d\n",plans);
}
}
int main(){
    mng::diagnostics::setScheduleControl(nullptr);
    assert(command("renderer_schedule status").find("unavailable")!=std::string::npos);
    mng::diagnostics::setScheduleControl(schedule);
    const char *names[]{"adaptive","batch","present","invalidate","reads","descriptors","images","pipeline","plans","flush"};
    for(unsigned target=0;target<10;++target){
        const auto request=std::string("renderer_schedule ")+names[target]+(target==1?" 1024":" on");
        assert(command(request).find("error=")==std::string::npos);
        for(unsigned i=0;i<10;++i)assert(received[i]==(i==target?(target==1?1024:1):-1));
    }
    assert(command("renderer_schedule plans off").find("preparation_plan=0")!=std::string::npos);
    assert(command("renderer_schedule plans on").find("preparation_plan=1")!=std::string::npos);
    command("renderer_schedule status");for(auto value:received)assert(value==-1);
    for(const char *request:{"plans maybe","plans 1","plans on extra","plans","batch 65537","status extra"})
        assert(command(std::string("renderer_schedule ")+request).find("invalid_schedule")!=std::string::npos);
    mng::diagnostics::setScheduleControl([](int,int,int,int,int,int,int,int,int,int,char*,unsigned){return -1;});
    assert(command("renderer_schedule plans off").find("retryable=1")!=std::string::npos);
    mng::diagnostics::setScheduleControl(nullptr);
    const auto directory=std::filesystem::temp_directory_path()/(
        "mng-guest-profile-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    mng::profile::initialize("mng-guest-profile",directory.string(),"title=test");
    assert(command("profiler_guest status").find("guest=0\n")!=std::string::npos);
    assert(command("profiler_guest status").find("condition_svc_1c_1d")!=std::string::npos);
    for(unsigned svc=0;svc<256;++svc)assert(bool(mng_profile_guest_svc(svc))==(svc==0x1c || svc==0x1d));
    assert(command("profiler_guest start").find("guest_epoch=1\n")!=std::string::npos);
    assert(!mng_profile_guest_epoch()); // Arming without a recording emits no events.
    assert(command("profiler_guest start").find("guest_epoch=1\n")!=std::string::npos);
    assert(command("profiler_guest stop").find("guest_epoch=2\n")!=std::string::npos);
    assert(command("profiler_guest start").find("guest_epoch=3\n")!=std::string::npos);
    assert(command("profiler_guest start extra").find("invalid_guest_action")!=std::string::npos);
    assert(command("profiler_guest unknown").find("invalid_guest_action")!=std::string::npos);
    assert(command("profiler_guest stop").find("guest=0\n")!=std::string::npos);
    assert(command("profiler_address status").find("address=0\n")!=std::string::npos);
    assert(command("profiler_address start").find("address_epoch=1\n")!=std::string::npos);
    assert(!mng_profile_address_epoch());
    assert(command("profiler_address stop").find("address_epoch=2\n")!=std::string::npos);
    assert(command("profiler_address invalid").find("invalid_address_action")!=std::string::npos);
    mng::diagnostics::setFlushImageControl([](int mode,char* out,unsigned bytes){return snprintf(out,bytes,"mode=%d\n",mode);});
    assert(command("renderer_flush_image on").find("mode=1\n")!=std::string::npos);
    assert(command("renderer_flush_image off").find("mode=0\n")!=std::string::npos);
    assert(command("renderer_flush_image status").find("mode=-1\n")!=std::string::npos);
    assert(command("renderer_flush_image invalid").find("invalid_flush_image_action")!=std::string::npos);
    mng::diagnostics::setFlushImageControl(nullptr);
    assert(command("renderer_invalidate status").find("invalidate_unavailable")!=std::string::npos);
    mng::diagnostics::setInvalidateControl([](int mode,char* out,unsigned bytes){return snprintf(out,bytes,"mode=%d\n",mode);});
    assert(command("renderer_invalidate on").find("mode=1\n")!=std::string::npos);
    assert(command("renderer_invalidate off").find("mode=0\n")!=std::string::npos);
    assert(command("renderer_invalidate status").find("mode=-1\n")!=std::string::npos);
    assert(command("renderer_invalidate invalid").find("invalid_invalidate_action")!=std::string::npos);
    assert(command("renderer_invalidate on extra").find("invalid_invalidate_action")!=std::string::npos);
    mng::diagnostics::setInvalidateControl([](int,char*,unsigned){return -1;});
    assert(command("renderer_invalidate on").find("emulation_session_busy")!=std::string::npos);
    mng::diagnostics::setInvalidateControl(nullptr);
    mng::diagnostics::setFrontLockControl([](int action,char* out,unsigned bytes){return snprintf(out,bytes,"action=%d\n",action);});
    mng::diagnostics::setPublicationControl([](int action,char* out,unsigned bytes){return snprintf(out,bytes,"action=%d\n",action);});
    assert(command("renderer_publication on").find("action=1\n")!=std::string::npos);
    assert(command("renderer_publication off").find("action=0\n")!=std::string::npos);
    assert(command("renderer_publication status").find("action=-1\n")!=std::string::npos);
    assert(command("renderer_publication invalid").find("invalid_publication_action")!=std::string::npos);
    assert(command("frontlock_status on").find("action=1\n")!=std::string::npos);
    assert(command("frontlock_status reset").find("action=2\n")!=std::string::npos);
    assert(command("frontlock_status off").find("action=0\n")!=std::string::npos);
    assert(command("frontlock_status status").find("action=-1\n")!=std::string::npos);
    assert(command("frontlock_status reset extra").find("invalid_frontlock_action")!=std::string::npos);
    mng::diagnostics::setFrontLockControl(nullptr);
    assert(command("lane_status status").find("lane_unavailable")!=std::string::npos);
    mng::diagnostics::setLaneCallbacks([](int action,char* out,unsigned bytes){return snprintf(out,bytes,"action=%d\n",action);},nullptr);
    assert(command("lane_status on").find("action=1\n")!=std::string::npos);
    assert(command("lane_status reset").find("action=2\n")!=std::string::npos);
    assert(command("lane_status off").find("action=0\n")!=std::string::npos);
    assert(command("lane_status status").find("action=-1\n")!=std::string::npos);
    assert(command("lane_status on extra").find("invalid_lane_action")!=std::string::npos);
    mng::diagnostics::setLaneCallbacks([](int,char*,unsigned){return -1;},nullptr);
    assert(command("lane_status on").find("retryable=1")!=std::string::npos);
    mng::diagnostics::setLaneCallbacks(nullptr,nullptr);
    // Exercise the real SDK string interner, including multiple cores, rows,
    // and changing identities. Mock-counter tests cannot catch this contract.
    assert(command("profiler_ring start").find("recording=1")!=std::string::npos);
    // A cold SDK applies its route at the first source frame. Without this,
    // every emitter below is a no-op and cannot exercise string interning.
    spatial::ProfilerRing::FrameMark();
    assert(spatial::ProfilerRing::Enabled());
    // One selection covers preflight and the whole draw, including nested
    // helpers. Nested scopes must not advance or override the outer stride.
    assert(command("profiler_detail start 32").find("error=")==std::string::npos);
    unsigned selected=0;
    for(unsigned i=0;i<96;++i){
        mng::profile::CpuDetailDrawScope draw;
        const bool detail=mng::profile::detailedCpu();selected+=detail;
        {mng::profile::CpuDetailDrawScope nested;assert(mng::profile::detailedCpu()==detail);}
        assert(mng::profile::detailedCpu()==detail);
    }
    assert(selected==3);
    command("profiler_detail stop");
    {mng::profile::CpuDetailDrawScope draw;assert(!mng::profile::detailedCpu());}
    mng_lane_accounting lanes{};mng_lane_reset(&lanes,1);
    mng_lane_find(&lanes,1,0x8001);mng_lane_find(&lanes,2,0x20004);
    const uint64_t zero[3]{};
    for(unsigned frame=1;frame<=4;++frame){
        lanes.rows[0].handle=frame;lanes.rows[0].held[0]+=100;
        lanes.rows[1].held[1]+=200;
        mng_lane_emit(&lanes,frame,1000,10,0,zero,zero);
        mng_lane_event event{frame,0,MNG_LANE_OWNER,0,0,0,0};
        auto packet=mng_lane_packet_begin(&lanes,frame,1000,(unsigned)frame);
        mng_lane_packet_event(&packet,&event);
        event.row=-1;mng_lane_packet_event(&packet,&event);
        mng_lane_packet_end(&packet);
    }
    assert(command("profiler_ring stop").find("recording=0")!=std::string::npos);
    puts("Shared renderer controls and default-off Guest diagnostic epochs passed");
}
