// SPDX-License-Identifier: MIT
#include "render_record_vulkan.h"
#include "../runtime/render_record.h"
#include "../runtime/port_runtime.h"
#include <chrono>
#include <memory>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
std::unique_ptr<mng::port::RenderRecord> recorder;
PFN_vkQueueInsertDebugUtilsLabelEXT queue_label;
PFN_vkCmdInsertDebugUtilsLabelEXT command_label;
VkQueue record_queue;
std::string label;
}
extern "C" int mng_render_record_initialize(VkInstance instance,VkPhysicalDevice physical,VkDevice device,VkQueue queue,const char* directory) {
    if(recorder||!directory||!*directory)return -1;
    queue_label=reinterpret_cast<PFN_vkQueueInsertDebugUtilsLabelEXT>(vkGetInstanceProcAddr(instance,"vkQueueInsertDebugUtilsLabelEXT"));
    command_label=reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(vkGetDeviceProcAddr(device,"vkCmdInsertDebugUtilsLabelEXT"));
    if(!queue_label||!command_label)return -2;
    unsigned char random[16];int fd=open("/dev/urandom",O_RDONLY|O_CLOEXEC);
    if(fd<0)return -3;const auto count=read(fd,random,sizeof random);close(fd);if(count!=sizeof random)return -3;
    char run[33];for(unsigned i=0;i<16;++i)snprintf(run+2*i,3,"%02x",random[i]);
    const auto path=std::string(directory)+"/render-record-"+run;
    if(mkdir(path.c_str(),0700))return -4;
    VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(physical,&props);
    const auto q=mng::port::record_json_string;
    std::string identity="{\"pid\":"+std::to_string(getpid())+",\"gpu\":"+q(props.deviceName)+
        ",\"vendor_id\":"+std::to_string(props.vendorID)+",\"device_id\":"+std::to_string(props.deviceID)+
        ",\"driver_version\":"+std::to_string(props.driverVersion)+",\"api_version\":"+std::to_string(props.apiVersion)+
        ",\"port_debug\":"+q(mng::port::debug_snapshot())+"}";
    record_queue=queue;
    mng::port::RenderRecord::Backend backend;
    backend.capture=[](bool begin){
        // The Pico RenderDoc layer consumes these queue markers. A marker sent
        // successfully is only a request; the host must verify the resulting RDC.
        VkDebugUtilsLabelEXT marker{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        marker.pLabelName=begin?"capture-marker,begin_capture":"capture-marker,end_capture";
        queue_label(record_queue,&marker);return true;
    };
    backend.milliseconds=[](){return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());};
    recorder=std::make_unique<mng::port::RenderRecord>(path,run,identity,std::move(backend));
    fprintf(stderr,"[render-record] ready run=%s directory=%s backend=pico-renderdoc-vulkan\n",run,path.c_str());
    return 0;
}
extern "C" int mng_render_record_request(uint64_t request,uint32_t frames,uint64_t bytes) {
    if(!recorder)return -1;
    bool accepted=recorder->arm(request,frames,bytes);
    fprintf(stderr,"[render-record] request=%llu accepted=%d frames=%u limit=%llu reason=%s\n",
        (unsigned long long)request,int(accepted),frames,(unsigned long long)bytes,recorder->error().c_str());
    return accepted?0:-2;
}
extern "C" void mng_render_record_present(uint64_t frame) {
    if(!recorder)return;
    const bool before=recorder->active(),armed=recorder->armed();recorder->present(frame);label.clear();
    if(before!=recorder->active()||armed)fprintf(stderr,"[render-record] frame=%llu active=%d path=%s error=%s\n",
        (unsigned long long)frame,int(recorder->active()),recorder->path().c_str(),recorder->error().c_str());
}
extern "C" void mng_render_record_shutdown(){label.clear();recorder.reset();}
extern "C" int mng_render_record_active(){return recorder&&recorder->active();}
extern "C" void mng_render_record_action(uint64_t submission,uint64_t command,uint32_t opcode,uint32_t bytes,const char* name,const char* detail) {
    label.clear();if(!recorder)return;
    const char* value=recorder->action(submission,command,opcode,bytes,name,detail);if(value)label=value;
}
extern "C" void mng_render_record_gpu_label(VkCommandBuffer command) {
    if(!recorder||!recorder->active()||label.empty()||!command_label)return;
    VkDebugUtilsLabelEXT marker{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};marker.pLabelName=label.c_str();
    command_label(command,&marker);
}
