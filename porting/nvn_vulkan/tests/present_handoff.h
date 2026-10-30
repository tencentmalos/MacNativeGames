// SPDX-License-Identifier: MIT
// Included by the synthetic Vulkan probe. No window, assets or guest code.
#include <future>
extern "C" uint64_t mng_nvn_present_ticket();
extern "C" void mng_nvn_wait_present(uint64_t);
namespace {
struct PresentProbe {
    VkDevice device{};VkQueue queue{};VkCommandPool pool{};
    VkCommandBuffer command{};VkBuffer buffer{};VkDeviceMemory memory{};VkFence fence{};
    uint8_t *mapped{};std::promise<void> waiting,release;
    std::shared_future<void> gate=release.get_future().share();
};
PresentProbe *present_probe;
int present_probe_submit(void*,void*,VkImage source,VkFormat format,uint32_t width,uint32_t height,const int32_t*,int32_t) {
    auto &p=*present_probe;
    need(format==VK_FORMAT_R8G8B8A8_UNORM && width==64 && height==32,"Synthetic present extent/format");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkcheck(vkBeginCommandBuffer(p.command,&bi));
    mng_nvn_prepare_present_source(p.command,source);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};
    vkCmdCopyImageToBuffer(p.command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,p.buffer,1,&copy);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(p.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    vkcheck(vkEndCommandBuffer(p.command));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&p.command;
    vkcheck(vkQueueSubmit(p.queue,1,&si,p.fence));return 0;
}
void present_probe_wait() {
    auto &p=*present_probe;p.waiting.set_value();
    // Keep completion pending even when this tiny GPU copy finishes quickly.
    // Bound the gate so a failed progress assertion cannot hang shutdown.
    p.gate.wait_for(std::chrono::seconds(5));
    vkcheck(vkWaitForFences(p.device,1,&p.fence,VK_TRUE,10000000000ull));
}
void present_handoff_test(VkPhysicalDevice physical,VkDevice device,VkQueue queue,uint32_t family,
                          NVNdevice *nd,NVNqueue *nq,NVNmemoryPool *pool,NVNtexture *source) {
    PresentProbe p;p.device=device;p.queue=queue;present_probe=&p;
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pi.queueFamilyIndex=family;
    vkcheck(vkCreateCommandPool(device,&pi,nullptr,&p.pool));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ai.commandPool=p.pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
    vkcheck(vkAllocateCommandBuffers(device,&ai,&p.command));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};vkcheck(vkCreateFence(device,&fi,nullptr,&p.fence));
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=64*32*4;bi.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkcheck(vkCreateBuffer(device,&bi,nullptr,&p.buffer));
    VkMemoryRequirements requirements;vkGetBufferMemoryRequirements(device,p.buffer,&requirements);
    VkPhysicalDeviceMemoryProperties memory;vkGetPhysicalDeviceMemoryProperties(physical,&memory);
    uint32_t type=0;
    while(type<memory.memoryTypeCount && (!(requirements.memoryTypeBits&(1u<<type)) ||
        (memory.memoryTypes[type].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))!=
        (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)))++type;
    need(type<memory.memoryTypeCount,"Synthetic present coherent readback memory");
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=type;
    vkcheck(vkAllocateMemory(device,&allocation,nullptr,&p.memory));vkcheck(vkBindBufferMemory(device,p.buffer,p.memory,0));
    vkcheck(vkMapMemory(device,p.memory,0,requirements.size,0,reinterpret_cast<void**>(&p.mapped)));
    mng_nvn_set_present_wait(present_probe_wait);
    mng_nvn_set_present_source_commands();
    const char *old=getenv("MNG_PRESENT_WORKER");std::string prior=old?old:"";setenv("MNG_PRESENT_WORKER","1",1);
    NVNtexture other{};NVNtextureBuilder tb{};
    NVN(nvnTextureBuilderSetDefaults)(&tb);NVN(nvnTextureBuilderSetDevice)(&tb,nd);
    NVN(nvnTextureBuilderSetSize2D)(&tb,64,32);NVN(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8);
    NVN(nvnTextureBuilderSetStorage)(&tb,pool,2u<<20);need(NVN(nvnTextureInitialize)(&other,&tb),"Synthetic second image");
    NVNwindowBuilder wb{};NVNwindow window{};NVNtexture *textures[]{source,&other};
    NVN(nvnWindowBuilderSetDefaults)(&wb);NVN(nvnWindowBuilderSetDevice)(&wb,nd);
    NVN(nvnWindowBuilderSetNativeWindow)(&wb,reinterpret_cast<NVNnativeWindow>(uintptr_t(1)));
    NVN(nvnWindowBuilderSetTextures)(&wb,2,textures);NVN(nvnWindowBuilderSetPresentInterval)(&wb,0);
    need(NVN(nvnWindowInitialize)(&window,&wb),"Synthetic window initialization");
    int primed=-1;NVN(nvnQueueAcquireTexture)(nq,&window,&primed);
    need(primed==0,"Initial acquired image identity");
    NVNcommandBuffer cb{};alignas(8) uint8_t control[128]{};
    need(NVN(nvnCommandBufferInitialize)(&cb,nd),"Present clear commands");
    auto clear=[&](NVNtexture *t) {
        NVN(nvnCommandBufferAddControlMemory)(&cb,control,sizeof control);
        NVN(nvnCommandBufferBeginRecording)(&cb);
        NVNcopyRegion region{0,0,0,64,32,1};const float green[]{0,1,0,1};
        NVN(nvnCommandBufferClearTexture)(&cb,t,nullptr,&region,green,15);
        auto handle=NVN(nvnCommandBufferEndRecording)(&cb);NVN(nvnQueueSubmitCommands)(nq,1,&handle);
    };
    // Both rendering slots are pending. Presentation must use its own command
    // buffer, retaining the source while later translation can retire slots.
    clear(source);NVN(nvnQueueFlush)(nq);clear(&other);NVN(nvnQueueFlush)(nq);
    mng_nvn_ordered_probe_drain();
    auto waiting=p.waiting.get_future();bool prepared=false;
    {
        SyncWaitGate busy_slots;
        NVN(nvnQueuePresentTexture)(nq,&window,0);
        prepared=waiting.wait_for(std::chrono::seconds(1))==std::future_status::ready;
        busy_slots.pending=false;busy_slots.release.set_value();
        // Leave no hook active while the completion callback resumes.
        waiting.wait();
    }
    const auto ticket=mng_nvn_present_ticket();need(ticket!=0,"Present ticket missing");
    auto unrelated=std::async(std::launch::async,[&]{int index=-1;NVN(nvnQueueAcquireTexture)(nq,&window,&index);return index;});
    const bool independent_image=unrelated.wait_for(std::chrono::seconds(1))==std::future_status::ready;
    if(!independent_image)p.release.set_value();
    need(unrelated.get()==1 && independent_image,"Acquire waited for an unrelated image's present");
    NVNsync acquired_sync{};NVN(nvnSyncInitialize)(&acquired_sync,nd);
    auto acquired=std::async(std::launch::async,[&]{int index=-1;NVN(nvnWindowAcquireTexture)(&window,&acquired_sync,&index);return index;});
    const bool image_wait=acquired.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout;
    auto progress=std::async(std::launch::async,[&]{clear(&other);NVN(nvnTextureFinalize)(source);});
    const bool advanced=progress.wait_for(std::chrono::seconds(1))==std::future_status::ready;
    p.release.set_value();progress.get();mng_nvn_wait_present(ticket);
    need(acquired.get()==0 && image_wait,"Acquire completed before its own present image");
    need(NVN(nvnSyncWait)(&acquired_sync,0)==NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED,"Acquire sync not published after image completion");
    NVN(nvnSyncFinalize)(&acquired_sync);
    need(prepared,"Present source barrier waited for an NVN arena slot");
    need(advanced,"Next translation blocked by present completion wait");
    for(unsigned i=0;i<64*32*4;i+=4)need(p.mapped[i]==0 && p.mapped[i+1]==255 && p.mapped[i+2]==0 && p.mapped[i+3]==255,
        "Present source lifetime/readback changed during concurrent translation");
    NVN(nvnWindowFinalize)(&window);NVN(nvnCommandBufferFinalize)(&cb);NVN(nvnTextureFinalize)(&other);
    if(old)setenv("MNG_PRESENT_WORKER",prior.c_str(),1);else unsetenv("MNG_PRESENT_WORKER");
    vkUnmapMemory(device,p.memory);vkDestroyBuffer(device,p.buffer,nullptr);vkFreeMemory(device,p.memory,nullptr);
    vkDestroyFence(device,p.fence,nullptr);vkDestroyCommandPool(device,p.pool,nullptr);present_probe=nullptr;
    fprintf(stderr,"[probe] exact per-image Acquire tickets and acquire sync; present completion releases engine lock; next translation/source finalization progress; 2048 copied pixels retained\n");
}
}
