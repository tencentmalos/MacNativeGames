// SPDX-License-Identifier: MIT
#include "vulkan.h"
#include "sdf_vulkan.h"
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <vector>
#include <cstdio>
#include <cstdlib>
namespace mng::overlay {
namespace {
VkPipeline sdf_pipeline{};
VkDevice device;VkRenderPass pass;VkExtent2D extent;
std::vector<VkImageView> views;std::vector<VkFramebuffer> buffers;
void check(VkResult result){if(result<0){std::fprintf(stderr,"[overlay] Vulkan failure %d\n",result);std::abort();}}
}
void initializeVulkan(const Config& config,VkInstance instance,VkPhysicalDevice gpu,VkDevice dev,VkQueue queue,uint32_t family,VkFormat format,VkExtent2D size,std::span<const VkImage> images) {
    device=dev;extent=size;
    VkAttachmentDescription attachment{};attachment.format=format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;attachment.initialLayout=attachment.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.colorAttachmentCount=1;subpass.pColorAttachments=&color;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};ci.attachmentCount=1;ci.pAttachments=&attachment;ci.subpassCount=1;ci.pSubpasses=&subpass;check(vkCreateRenderPass(device,&ci,nullptr,&pass));
    for(auto image:images){VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkImageView view;check(vkCreateImageView(device,&vi,nullptr,&view));views.push_back(view);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fi.renderPass=pass;fi.attachmentCount=1;fi.pAttachments=&view;fi.width=size.width;fi.height=size.height;fi.layers=1;VkFramebuffer buffer;check(vkCreateFramebuffer(device,&fi,nullptr,&buffer));buffers.push_back(buffer);}
    IMGUI_CHECKVERSION();ImGui::CreateContext();
    ImGui_ImplVulkan_InitInfo init{};init.ApiVersion=VK_API_VERSION_1_1;init.Instance=instance;init.PhysicalDevice=gpu;init.Device=device;init.QueueFamily=family;init.Queue=queue;init.RenderPass=pass;init.MinImageCount=2;init.ImageCount=images.size();init.MSAASamples=VK_SAMPLE_COUNT_1_BIT;init.DescriptorPoolSize=128;init.CheckVkResultFn=check;
    if(!ImGui_ImplVulkan_Init(&init))std::abort();
    sdf_pipeline=createSdfPipeline(device,pass);shared().initialize(config,size.width,size.height,sdf_pipeline!=VK_NULL_HANDLE);
}
void drawVulkan(VkCommandBuffer command,uint32_t image) {
    ImGui_ImplVulkan_NewFrame();shared().draw();auto* data=ImGui::GetDrawData();
    // Font texture uploads must happen outside the render pass. They use this
    // same selected driver and queue; no platform Vulkan loader is introduced.
    bool sdf_ready=sdf_pipeline!=VK_NULL_HANDLE;
    if(data&&data->Textures)for(auto* texture:*data->Textures){
        if(sdf_pipeline){const bool retiring=texture->Status==ImTextureStatus_WantDestroy||texture->Status==ImTextureStatus_Destroyed;
            if(!updateSdfTexture(texture)&&!retiring)sdf_ready=false;}
        else if(texture->Status!=ImTextureStatus_OK)ImGui_ImplVulkan_UpdateTexture(texture);
    }
    if(sdf_pipeline && !sdf_ready){std::fprintf(stderr,"[overlay] SDF atlas upload failed\n");std::abort();}
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=pass;begin.framebuffer=buffers[image];begin.renderArea.extent=extent;
    vkCmdBeginRenderPass(command,&begin,VK_SUBPASS_CONTENTS_INLINE);ImGui_ImplVulkan_RenderDrawData(data,command,sdf_pipeline);shared().sdfSubmitted(sdf_ready);vkCmdEndRenderPass(command);
}
}
