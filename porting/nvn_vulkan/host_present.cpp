// SPDX-License-Identifier: MIT
#include <vulkan/vulkan.h>
#include "nvn_vulkan.h"
#include "host_platform.h"
#include <algorithm>
#include <vector>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "render_record_vulkan.h"
#include "vulkan.h"

#include "profiler.h"
#include "debugbus.h"
#include "performance_hint.h"
#include "nvn_vulkan.h"
#include <ctime>
#include <fstream>
extern "C" {
struct xbg_frame_counters {uint64_t pipelines,pipeline_ns,shader_loads,shader_load_ns,uploads,reuploads,upload_bytes,upload_ns;};
void xbg_game_frame_counters(xbg_frame_counters*);
void xbg_game_set_frame(uint64_t);
double xbg_game_gpu_ms(uint64_t);
void runtime_input_clear();
}


extern "C" [[noreturn]] void runtime_fail(const char*);
extern "C" int mng_engine_publication_counters(uint64_t*,uint64_t*);
extern "C" void mng_nvn_texture_counters(uint64_t*,uint64_t*);
namespace {
VkPhysicalDevice gpu;VkDevice device;VkQueue queue;VkSurfaceKHR surface;VkSwapchainKHR swapchain;
VkCommandPool pool;VkCommandBuffer command;VkExtent2D extent;VkFormat format;
std::vector<VkImage> images;uint64_t frames;
// Acquired semaphore/command/fence are reused only after this copy completes.
// Present wait semaphores belong to swapchain images: the copy fence does not
// prove the presentation engine has consumed its wait semaphore.
VkSemaphore acquired;std::vector<VkSemaphore> present_ready;VkFence copy_fence;
bool completion_pending;
mng::overlay::FrameSample pending_sample;
std::chrono::steady_clock::time_point pending_begin;
int pending_interval;
xbg_game_stats previous_stats{};xbg_frame_counters previous_counters{};
uint64_t previous_cpu{};
auto previous_present=std::chrono::steady_clock::now();
uint64_t cpu_time(){timespec t{};clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t);return uint64_t(t.tv_sec)*1000000000ull+t.tv_nsec;}
#ifndef NDEBUG
bool record_enabled;
uint64_t capture_request;
void capture_boundary() {
    if(!record_enabled)return;
    char value[256]{};mng_host_option("debug.mng.capture_frame",value,sizeof value);
    unsigned long long request=0;unsigned count=1,mib=64;char extra=0;
    int fields=sscanf(value,"%llu:%u:%u%c",&request,&count,&mib,&extra);
    if(fields!=1&&fields!=3)request=0;
    if(request && request!=capture_request){
        capture_request=request;mng_render_record_request(request,count,uint64_t(mib)<<20);
    }
    mng_render_record_present(frames);
}
#endif
void check(VkResult result) {if(result!=VK_SUCCESS){char text[100];snprintf(text,sizeof text,"Native Vulkan present failed: %d",result);runtime_fail(text);}}
void barrier(VkImage image,VkImageLayout from,VkImageLayout to,VkAccessFlags access) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=from;b.newLayout=to;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;
    b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};b.dstAccessMask=access;b.srcAccessMask=from==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
}
}
extern "C" void mng_present_initialize(VkInstance instance,VkPhysicalDevice physical,VkDevice dev,VkQueue q,uint32_t family) {
    auto *window=mng_host_window();if(!window)return;
    gpu=physical;device=dev;queue=q;
#ifndef NDEBUG
    // Opt-in Pico RenderDoc application-controlled frames prevent its secondary
    // instance fallback from treating each queue submission as a complete frame.
    char capture[256]{};mng_host_option("debug.mng.capture_enable",capture,sizeof capture);
    record_enabled=!strcmp(capture,"1");
#endif
    check(mng_host_create_surface(instance,&surface));
    VkBool32 supported;check(vkGetPhysicalDeviceSurfaceSupportKHR(gpu,family,surface,&supported));if(!supported)runtime_fail("Graphics queue cannot present to native surface");
    VkSurfaceCapabilitiesKHR caps;check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu,surface,&caps));
    if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT))runtime_fail("Native swapchain lacks transfer destination usage");
    uint32_t n=0;check(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu,surface,&n,nullptr));if(!n)runtime_fail("No surface formats");
    std::vector<VkSurfaceFormatKHR> formats(n);check(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu,surface,&n,formats.data()));auto selected=formats[0];
    for(auto candidate:formats)if(candidate.format==VK_FORMAT_R8G8B8A8_UNORM){selected=candidate;break;}
    format=selected.format;extent=caps.currentExtent;
    if(extent.width==UINT32_MAX)extent=mng_host_surface_extent();
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};ci.surface=surface;
    ci.minImageCount=std::max(2u,caps.minImageCount);if(caps.maxImageCount)ci.minImageCount=std::min(ci.minImageCount,caps.maxImageCount);
    ci.imageFormat=format;ci.imageColorSpace=selected.colorSpace;ci.imageExtent=extent;ci.imageArrayLayers=1;
    if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))runtime_fail("Native swapchain lacks overlay color attachment usage");
    ci.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.preTransform=(caps.supportedTransforms&VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)?VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR:caps.currentTransform;
    ci.compositeAlpha=VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha&-caps.supportedCompositeAlpha);
    uint32_t mode_count=0;check(vkGetPhysicalDeviceSurfacePresentModesKHR(gpu,surface,&mode_count,nullptr));
    std::vector<VkPresentModeKHR> modes(mode_count);
    check(vkGetPhysicalDeviceSurfacePresentModesKHR(gpu,surface,&mode_count,modes.data()));
    modes.resize(mode_count);
    // Guest image reuse already implements the NVN interval at AcquireTexture.
    // An explicit unsynchronized comparison must use IMMEDIATE, not silently
    // fall back to FIFO or mailbox. The ordinary default remains FIFO.
    char requested_mode[256]{};mng_host_option("debug.mng.present_mode",requested_mode,sizeof requested_mode);
    const bool mailbox=std::find(modes.begin(),modes.end(),VK_PRESENT_MODE_MAILBOX_KHR)!=modes.end();
    const bool immediate=std::find(modes.begin(),modes.end(),VK_PRESENT_MODE_IMMEDIATE_KHR)!=modes.end();
    ci.presentMode=mailbox&&!strcmp(requested_mode,"mailbox")?VK_PRESENT_MODE_MAILBOX_KHR:VK_PRESENT_MODE_FIFO_KHR;
    if(!strcmp(requested_mode,"immediate")) {
        if(!immediate)runtime_fail("VSync off requested, but Vulkan IMMEDIATE present mode is unavailable");
        ci.presentMode=VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    ci.clipped=VK_TRUE;check(vkCreateSwapchainKHR(device,&ci,nullptr,&swapchain));
    check(vkGetSwapchainImagesKHR(device,swapchain,&n,nullptr));images.resize(n);check(vkGetSwapchainImagesKHR(device,swapchain,&n,images.data()));
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=family;pi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;check(vkCreateCommandPool(device,&pi,nullptr,&pool));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;check(vkAllocateCommandBuffers(device,&ai,&command));
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    check(vkCreateSemaphore(device,&si,nullptr,&acquired));present_ready.resize(n);
    for(auto &sem:present_ready)check(vkCreateSemaphore(device,&si,nullptr,&sem));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(vkCreateFence(device,&fi,nullptr,&copy_fence));
    mng::overlay::Config overlay;
    overlay.directory=getenv("TOTK_PROFILE_DIR")?getenv("TOTK_PROFILE_DIR"):".";
    overlay.title="BOTW 1.6.0";overlay.chinese=!getenv("TOTK_LANG")||!strncmp(getenv("TOTK_LANG"),"zh",2);
    std::ifstream identity(overlay.directory+"/driver-status.txt");overlay.driver.assign(std::istreambuf_iterator<char>(identity),{});
    overlay.font_path=mng_host_font();
    overlay.density=getenv("MNG_OVERLAY_DENSITY")?strtof(getenv("MNG_OVERLAY_DENSITY"),nullptr):
        std::clamp(std::min(float(extent.width)/1280.f,float(extent.height)/720.f),1.f,4.f);
    mng::overlay::initializeVulkan(overlay,instance,gpu,device,queue,family,format,extent,images);
    previous_cpu=cpu_time();previous_present=std::chrono::steady_clock::now();xbg_game_set_frame(1);
    fprintf(stderr,"[native-present] Surface %ux%u, %u swapchain images, transform=%u current=%u supported=%u\n",extent.width,extent.height,n,unsigned(ci.preTransform),unsigned(caps.currentTransform),unsigned(caps.supportedTransforms));
    fprintf(stderr,"[native-present] host_mode=%s mailbox_supported=%d immediate_supported=%d requested=%s pacing=guest_acquire\n",ci.presentMode==VK_PRESENT_MODE_IMMEDIATE_KHR?"immediate":ci.presentMode==VK_PRESENT_MODE_MAILBOX_KHR?"mailbox":"fifo",mailbox,immediate,requested_mode[0]?requested_mode:"default");
}
extern "C" int mng_present_image(void*,void*,VkImage source,VkFormat source_format,uint32_t width,uint32_t height,const int32_t crop[4],int32_t interval) {
    spatial::LiteTraceScope profile_scope("Host.Present");
    const auto present_begin=std::chrono::steady_clock::now();
    if(!swapchain)runtime_fail("Game reached present but the standalone probe has no native window");
    VkFormatProperties src,dst;vkGetPhysicalDeviceFormatProperties(gpu,source_format,&src);vkGetPhysicalDeviceFormatProperties(gpu,format,&dst);
    if(!(src.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(dst.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT))runtime_fail("Present formats do not support blit");
    if(completion_pending)runtime_fail("Previous present copy has not completed");
    check(vkResetFences(device,1,&copy_fence));
    uint32_t index;VkResult result;
    {spatial::LiteTraceScope scope("Host.Present.AcquireImage");result=vkAcquireNextImageKHR(device,swapchain,5000000000ull,acquired,VK_NULL_HANDLE,&index);}
    if(result!=VK_SUBOPTIMAL_KHR)check(result);
    const auto hint_begin=mng::port::performance_hint_begin();
    const VkSemaphore complete=present_ready.at(index);
    check(vkResetCommandBuffer(command,0));VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(vkBeginCommandBuffer(command,&bi));
    mng::profile::collectGpu(command);mng::profile::GpuZone profile_zone(command);mng::profile::beginGpu(profile_zone,command,"Host.Present");
    mng_nvn_prepare_present_source(command,source);
    barrier(images[index],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT);
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkClearColorValue black{};
    vkCmdClearColorImage(command,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
    int x=0,y=0,w=width,h=height;if(crop[2]&&crop[3]){x=crop[0];y=crop[1];w=crop[2];h=crop[3];}
    if(x<0||y<0||w<=0||h<=0||uint64_t(x)+w>width||uint64_t(y)+h>height)runtime_fail("Present crop outside image");
    double scale=std::min(double(extent.width)/w,double(extent.height)/h);int dw=int(w*scale),dh=int(h*scale);
    VkImageBlit blit{};blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    blit.srcOffsets[0]={x,y,0};blit.srcOffsets[1]={x+w,y+h,1};
    int dx=(int(extent.width)-dw)/2,dy=(int(extent.height)-dh)/2;blit.dstOffsets[0]={dx,dy,0};blit.dstOffsets[1]={dx+dw,dy+dh,1};
    // Serialize the clear and blit writes to the same swapchain image.
    barrier(images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdBlitImage(command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
    barrier(images[index],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    static bool captured=false;
    {spatial::LiteTraceScope scope("Host.Present.Overlay");mng::overlay::drawVulkan(command,index);}
    if(captured!=mng::overlay::shared().capturesInput()){captured=mng::overlay::shared().capturesInput();mng_host_capture_input(captured);}
    barrier(images[index],VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,0);
    mng::profile::endGpu(profile_zone);mng::profile::collectGpu(command);
    check(vkEndCommandBuffer(command));VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.waitSemaphoreCount=1;submit.pWaitSemaphores=&acquired;submit.pWaitDstStageMask=&stage;submit.commandBufferCount=1;submit.pCommandBuffers=&command;submit.signalSemaphoreCount=1;submit.pSignalSemaphores=&complete;
    {spatial::LiteTraceScope scope("Host.Present.Submit");check(vkQueueSubmit(queue,1,&submit,copy_fence));}VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};present.waitSemaphoreCount=1;present.pWaitSemaphores=&complete;present.swapchainCount=1;present.pSwapchains=&swapchain;present.pImageIndices=&index;
    mng::port::performance_hint_end(mng::port::HintLane::Present,hint_begin);
    {spatial::LiteTraceScope scope("Host.Present.QueuePresent");result=vkQueuePresentKHR(queue,&present);}if(result!=VK_SUBOPTIMAL_KHR)check(result);
    // GPU completion is a separate phase. The engine can translate and submit
    // newer work while this copy runs; all queue API calls above stay serialized.
    if(++frames<=5 || frames%300==0)fprintf(stderr,"[native-present] frame=%llu source=%ux%u crop=%d,%d,%dx%d surface=%ux%u interval=%d\n",(unsigned long long)frames,width,height,x,y,w,h,extent.width,extent.height,interval);
#ifndef NDEBUG
    capture_boundary();
#endif
    // Snapshot renderer counters before unlocking. Completion must not query
    // mutable engine state or accidentally attribute next-frame draws here.
    mng::overlay::FrameSample sample;sample.frame=frames;
    if(frames>8){sample.gpu_frame=frames-8;const auto gpu_ms=xbg_game_gpu_ms(sample.gpu_frame);if(gpu_ms>=0)sample.gpu_ms=gpu_ms;}
    xbg_game_stats stats{};xbg_game_get_stats(&stats);xbg_frame_counters counters{};xbg_game_frame_counters(&counters);
    sample.draws=stats.draws-previous_stats.draws;sample.dispatches=stats.dispatches-previous_stats.dispatches;
    sample.pipelines=counters.pipelines-previous_counters.pipelines;sample.pipeline_ms=double(counters.pipeline_ns-previous_counters.pipeline_ns)/1e6;
    sample.shaders=counters.shader_loads-previous_counters.shader_loads;sample.shader_ms=double(counters.shader_load_ns-previous_counters.shader_load_ns)/1e6;
    sample.upload_bytes=counters.upload_bytes-previous_counters.upload_bytes;sample.upload_ms=double(counters.upload_ns-previous_counters.upload_ns)/1e6;
    mng_nvn_resources resources{};
    if(!mng_nvn_resource_stats(&resources)) {
        spatial::LiteTrace::counter("Native.HostObjects",resources.host_objects);
        spatial::LiteTrace::counter("Native.ImageIdentities",resources.image_identities);
        spatial::LiteTrace::counter("Native.PhysicalImages",resources.physical_images);
        spatial::LiteTrace::counter("Native.TextureSnapshots",resources.texture_snapshots);
        spatial::LiteTrace::counter("Native.CommandBytes",resources.command_bytes);
        spatial::LiteTrace::counter("Native.TextureInitializations",resources.texture_initializations);
    }
    // Present-boundary deltas; a failed snapshot explicitly invalidates the
    // baseline rather than attributing multiple frames to the next sample.
    uint64_t epoch{},rebuilds{},inits{},finis{};
    static uint64_t last_epoch{},last_rebuilds{},last_inits{},last_finis{};
    static bool valid=false;
    const bool counters_captured=mng_engine_publication_counters(&epoch,&rebuilds)==0;
    mng_nvn_texture_counters(&inits,&finis);
    const bool delta_valid=counters_captured && valid && epoch>=last_epoch && rebuilds>=last_rebuilds && inits>=last_inits && finis>=last_finis;
    spatial::LiteTrace::counter("Native.PublicationDeltaValid",delta_valid);
    if(delta_valid){
        spatial::LiteTrace::counter("Native.ImagesEpochDelta",epoch-last_epoch);
        spatial::LiteTrace::counter("Native.ImageIndexRebuildsDelta",rebuilds-last_rebuilds);
        spatial::LiteTrace::counter("Native.TextureInitializationsDelta",inits-last_inits);
        spatial::LiteTrace::counter("Native.TextureFinalizationsDelta",finis-last_finis);
    }
    last_epoch=epoch;last_rebuilds=rebuilds;last_inits=inits;last_finis=finis;valid=counters_captured;
    spatial::LiteTrace::counter("Native.PresentInterval",interval);
    spatial::LiteTrace::counter("Native.Draws",sample.draws);
    spatial::LiteTrace::counter("Native.Dispatches",sample.dispatches);
    spatial::LiteTrace::counter("Native.Pipelines",sample.pipelines);
    spatial::LiteTrace::counter("Native.ShaderLoads",sample.shaders);
    spatial::LiteTrace::counter("Native.UploadBytes",sample.upload_bytes);
    spatial::LiteTrace::counter("Native.PipelineNs",counters.pipeline_ns-previous_counters.pipeline_ns);
    spatial::LiteTrace::counter("Native.UploadNs",counters.upload_ns-previous_counters.upload_ns);
    previous_stats=stats;previous_counters=counters;xbg_game_set_frame(frames+1);
    mng::profile::preparePresent();
    pending_sample=sample;pending_begin=present_begin;pending_interval=interval;completion_pending=true;
    return 0;
}
extern "C" void mng_present_wait() {
    if(!completion_pending)runtime_fail("No present copy to complete");
    {spatial::LiteTraceScope scope("Host.Present.CopyFence");
     check(vkWaitForFences(device,1,&copy_fence,VK_TRUE,10000000000ull));}
    const auto now=std::chrono::steady_clock::now();const auto cpu=cpu_time();
    auto sample=pending_sample;
    sample.frame_ms=std::chrono::duration<double,std::milli>(now-previous_present).count();
    sample.cpu_process_ms=double(cpu-previous_cpu)/1e6;
    sample.present_ms=std::chrono::duration<double,std::milli>(now-pending_begin).count();
    mng::profile::present(sample.frame,true);
    mng::diagnostics::frame({sample.frame,sample.gpu_frame,sample.draws,sample.dispatches,sample.pipelines,sample.shaders,sample.upload_bytes,
        sample.frame_ms,sample.cpu_process_ms,sample.present_ms,sample.gpu_ms.value_or(-1),sample.pipeline_ms,sample.shader_ms,sample.upload_ms,pending_interval});
    mng_host_presented();mng::overlay::shared().presented(sample);
    previous_cpu=cpu;previous_present=now;completion_pending=false;
}
