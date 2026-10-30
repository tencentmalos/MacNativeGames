// SPDX-License-Identifier: MIT
#include "render_record.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

namespace mng::port {
std::string record_json_string(const std::string& value) {
    std::string out="\"";
    for(unsigned char c:value) {
        if(c=='"'||c=='\\'){out+='\\';out+=char(c);}
        else if(c<32){char escaped[7];snprintf(escaped,sizeof escaped,"\\u%04x",c);out+=escaped;}
        else out+=char(c);
    }
    return out+'"';
}
RenderRecord::RenderRecord(std::string directory,std::string run,std::string identity,Backend backend)
    :directory_(std::move(directory)),run_(std::move(run)),identity_(std::move(identity)),backend_(std::move(backend)) {}
RenderRecord::~RenderRecord(){abort("owner_destroyed");}
bool RenderRecord::arm(uint64_t request,uint32_t frames,uint64_t max_bytes,uint64_t max_events,uint64_t timeout_ms) {
    if(active()||armed_)return false;
    if(!request||!frames||frames>8||max_bytes<8192||max_bytes>(128u<<20)||!max_events||max_events>1000000||
       !timeout_ms||timeout_ms>60000||run_.size()!=32||
       !std::all_of(run_.begin(),run_.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})||
       !backend_.capture||!backend_.milliseconds){error_="invalid_record_request";return false;}
    request_=request;frames_=frames;max_bytes_=max_bytes;max_events_=max_events;timeout_=timeout_ms;
    path_=directory_+"/capture-"+std::to_string(request)+".nvn.jsonl";
    bytes_=events_=done_=0;error_.clear();armed_=true;return true;
}
bool RenderRecord::write(const std::string& line,bool terminal) {
    // Keep space for a terminal record even when the event limit is exhausted.
    const uint64_t limit=terminal?max_bytes_:max_bytes_-4096;
    if(!file_||line.size()+1>limit||bytes_>limit-line.size()-1){error_="trace_size_limit";return false;}
    if(fwrite(line.data(),1,line.size(),file_)!=line.size()||fputc('\n',file_)==EOF){error_="trace_io_error";return false;}
    bytes_+=line.size()+1;return true;
}
void RenderRecord::present(uint64_t completed) {
    if(active()) {
        if(completed!=frame_){finish(false,"nonconsecutive_present");return;}
        if(!write("{\"type\":\"present\",\"frame\":"+std::to_string(completed)+"}")){finish(false,error_.c_str());return;}
        ++done_;
        if(backend_.milliseconds()>deadline_){finish(false,"record_timeout");return;}
        if(done_==frames_){finish(true,"window_recorded");return;}
        ++frame_;
    } else if(armed_) {
        armed_=false;
        const int fd=open(path_.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);
        if(fd<0){error_="trace_open: "+std::string(strerror(errno));return;}
        file_=fdopen(fd,"wb");if(!file_){close(fd);error_="trace_fdopen";return;}
        first_frame_=frame_=completed+1;deadline_=backend_.milliseconds()+timeout_;
        if(!write("{\"type\":\"header\",\"schema\":\"mng.nvn-index.v1\",\"run\":"+record_json_string(run_)+
            ",\"request\":"+std::to_string(request_)+",\"first_frame\":"+std::to_string(frame_)+
            ",\"frame_count\":"+std::to_string(frames_)+",\"identity\":"+record_json_string(identity_)+
            ",\"replay_backend\":\"renderdoc-vulkan\",\"resource_payload\":\"external_rdc_unverified\","
            "\"guest_pc\":null,\"guest_pc_reason\":\"translated_command_arena_only\"}")) {finish(false,error_.c_str());return;}
        backend_active_=backend_.capture(true);
        if(!backend_active_)finish(false,"capture_backend_unavailable");
    }
}
const char* RenderRecord::action(uint64_t submission,uint64_t address,uint32_t opcode,uint32_t size,const char* name,const char* detail) {
    if(!active())return nullptr;
    if(backend_.milliseconds()>deadline_){finish(false,"record_timeout");return nullptr;}
    if(events_>=max_events_){finish(false,"event_limit");return nullptr;}
    const uint64_t id=events_+1;
    char location[24];snprintf(location,sizeof location,"0x%llx",(unsigned long long)address);
    if(!write("{\"type\":\"action\",\"event\":"+std::to_string(id)+",\"frame\":"+std::to_string(frame_)+
        ",\"submission\":"+std::to_string(submission)+",\"command_address\":"+record_json_string(location)+
        ",\"opcode\":"+std::to_string(opcode)+",\"bytes\":"+std::to_string(size)+
        ",\"name\":"+record_json_string(name?name:"unknown")+",\"detail\":"+record_json_string(detail?detail:"")+"}")) {
        finish(false,error_.c_str());return nullptr;
    }
    events_=id;
    label_="macnative.nvn run="+run_+" capture="+std::to_string(request_)+" frame="+std::to_string(frame_)+
        " submission="+std::to_string(submission)+" event="+std::to_string(id)+" op="+std::to_string(opcode)+" "+(name?name:"unknown");
    return label_.c_str();
}
void RenderRecord::finish(bool complete,const char* reason) {
    const std::string why=reason?reason:"unknown"; // reason may alias error_.
    armed_=false;
    if(backend_active_&&!backend_.capture(false)){complete=false;error_="capture_end_failed";}
    backend_active_=false;
    if(file_) {
        if(!write("{\"type\":\"footer\",\"complete\":"+std::string(complete?"true":"false")+
            ",\"reason\":"+record_json_string(error_.empty()?why:error_)+",\"events\":"+std::to_string(events_)+
            ",\"completed_frames\":"+std::to_string(done_)+",\"rdc_verified\":false}",true))complete=false;
        if(fflush(file_)||fsync(fileno(file_)))error_="trace_flush_error";
        if(fclose(file_))error_="trace_close_error";
        file_=nullptr;
    }
    if(!complete&&error_.empty())error_=why;
    label_.clear();
}
void RenderRecord::abort(const char* reason) {if(active()||armed_)finish(false,reason);}
}
