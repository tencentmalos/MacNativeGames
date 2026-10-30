// SPDX-License-Identifier: MIT
#include "sdf_vulkan.h"
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <spatial/imgui/SdfFontAtlas.hpp>
#include <spatial/imgui/SdfTextShaders.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <unordered_map>
#include <vector>
namespace mng::overlay {
VkPipeline createSdfPipeline(VkDevice device, VkRenderPass pass) {
    const auto modules=spatial::imgui::getSdfTextShaderModules();
    if(!modules.available)return VK_NULL_HANDLE;
    VkShaderModule shaders[2]{};VkDescriptorSetLayout descriptors{};VkPipelineLayout layout{};VkPipeline result{};
    auto cleanup=[&]{for(auto shader:shaders)if(shader)vkDestroyShaderModule(device,shader,nullptr);
        if(layout)vkDestroyPipelineLayout(device,layout,nullptr);if(descriptors)vkDestroyDescriptorSetLayout(device,descriptors,nullptr);};
    for(unsigned i=0;i<2;++i){auto code=i?modules.fragment_spirv:modules.vertex_spirv;
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=code.size_bytes();ci.pCode=code.data();
        if(vkCreateShaderModule(device,&ci,nullptr,&shaders[i])!=VK_SUCCESS){cleanup();return {};}}
    // Compatible with the unmodified ImGui backend descriptor and push layouts.
    VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo di{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};di.bindingCount=1;di.pBindings=&binding;
    if(vkCreateDescriptorSetLayout(device,&di,nullptr,&descriptors)!=VK_SUCCESS){cleanup();return {};}
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT,0,16};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=1;li.pSetLayouts=&descriptors;li.pushConstantRangeCount=1;li.pPushConstantRanges=&push;
    if(vkCreatePipelineLayout(device,&li,nullptr,&layout)!=VK_SUCCESS){cleanup();return {};}
    VkPipelineShaderStageCreateInfo stages[2]{};
    for(unsigned i=0;i<2;++i){stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;stages[i].module=shaders[i];stages[i].pName="main";}
    VkVertexInputBindingDescription vb{0,sizeof(ImDrawVert),VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[]={{0,0,VK_FORMAT_R32G32_SFLOAT,offsetof(ImDrawVert,pos)},{1,0,VK_FORMAT_R32G32_SFLOAT,offsetof(ImDrawVert,uv)},{2,0,VK_FORMAT_R8G8B8A8_UNORM,offsetof(ImDrawVert,col)}};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};vi.vertexBindingDescriptionCount=1;vi.pVertexBindingDescriptions=&vb;vi.vertexAttributeDescriptionCount=3;vi.pVertexAttributeDescriptions=va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;rs.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blend{};blend.blendEnable=VK_TRUE;blend.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;blend.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;blend.colorBlendOp=VK_BLEND_OP_ADD;blend.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;blend.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;blend.alphaBlendOp=VK_BLEND_OP_ADD;blend.colorWriteMask=15;
    VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};bs.attachmentCount=1;bs.pAttachments=&blend;
    VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=2;dy.pDynamicStates=dynamic;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};ci.stageCount=2;ci.pStages=stages;ci.pVertexInputState=&vi;ci.pInputAssemblyState=&ia;ci.pViewportState=&vp;ci.pRasterizationState=&rs;ci.pMultisampleState=&ms;ci.pDepthStencilState=&ds;ci.pColorBlendState=&bs;ci.pDynamicState=&dy;ci.layout=layout;ci.renderPass=pass;
    if(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&ci,nullptr,&result)!=VK_SUCCESS)result={};
    cleanup();return result;
}
bool updateSdfTexture(ImTextureData* texture) {
    static std::unordered_map<int,std::vector<unsigned char>> atlases;
    const auto status=texture->Status;
    if(status==ImTextureStatus_OK)return atlases.contains(texture->UniqueID);
    if(status!=ImTextureStatus_WantCreate && status!=ImTextureStatus_WantUpdates){
        ImGui_ImplVulkan_UpdateTexture(texture);if(texture->Status==ImTextureStatus_Destroyed)atlases.erase(texture->UniqueID);return false;
    }
    if(texture->Format!=ImTextureFormat_RGBA32)return false;
    spatial::imgui::SdfFontAtlasConfig config;
    const auto shaders=spatial::imgui::getSdfTextShaderModules();config.edge_value=shaders.edge_value;
    auto& pixels=atlases[texture->UniqueID];pixels.resize(size_t(texture->Width)*texture->Height*4);
    const auto original_rect=texture->UpdateRect;
    auto expand=[&](ImTextureRect r,int p){const int x=std::max(0,int(r.x)-p),y=std::max(0,int(r.y)-p);
        return ImTextureRect{(unsigned short)x,(unsigned short)y,(unsigned short)(std::min(texture->Width,int(r.x+r.w)+p)-x),(unsigned short)(std::min(texture->Height,int(r.y+r.h)+p)-y)};};
    const int padding=int(std::ceil(config.spread_pixels))+1;
    // Read a second border so the uploaded border is not computed against a
    // fabricated crop boundary. Existing glyphs keep their original source.
    auto upload=status==ImTextureStatus_WantCreate?ImTextureRect{0,0,(unsigned short)texture->Width,(unsigned short)texture->Height}:expand(original_rect,padding);
    auto input=expand(upload,padding);
    std::vector<unsigned char> bitmap(size_t(input.w)*input.h*4);
    for(int y=0;y<input.h;++y)std::memcpy(bitmap.data()+size_t(y)*input.w*4,texture->GetPixelsAt(input.x,input.y+y),size_t(input.w)*4);
    auto sdf=spatial::imgui::BuildSdfFontAtlasRgba32(bitmap,input.w,input.h,config);
    if(!sdf.IsValid())return false;
    for(int y=0;y<upload.h;++y)std::memcpy(pixels.data()+(size_t(upload.y+y)*texture->Width+upload.x)*4,
        sdf.rgba32.data()+(size_t(upload.y-input.y+y)*input.w+upload.x-input.x)*4,size_t(upload.w)*4);
    auto* bitmap_pixels=texture->Pixels;texture->Pixels=pixels.data();texture->UpdateRect=upload;
    ImGui_ImplVulkan_UpdateTexture(texture);
    texture->Pixels=bitmap_pixels;texture->UpdateRect=original_rect;
    return texture->Status==ImTextureStatus_OK;
}
}
