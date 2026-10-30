// SPDX-License-Identifier: MIT
// Match Metal's memory-sampling runs without sampling a writable Vulkan
// attachment. Explicit guest boundaries and plain draws start a new run.
// Copies stay owned by the source Image, whose retirement already waits for
// its submitted uses. Reuse is ordered on the same queue with full barriers.
namespace mng_feedback {
uint64_t epoch=1,copies=0,draws=0;
std::array<Image*,8> current{};
unsigned current_count=0;
uint32_t sampled_attachments=0;
bool sampled_attachments_known=false;
bool enabled() {
    static const bool value=[] {const char *v=getenv("MNG_FEEDBACK_SNAPSHOT");return v && !strcmp(v,"1");}();
    return value;
}
void boundary(){current_count=0;++epoch;need(epoch!=0,"Feedback epoch overflow");}
Image &sample(Image &source) {
    for(unsigned i=0;i<current_count;++i)if(current[i]==&source)return *source.feedback_copy;
    return source;
}
void key(std::vector<uint64_t> &out) {
    out.push_back(current_count);
    for(unsigned i=0;i<current_count;++i){out.push_back(uint64_t(uintptr_t(current[i])));out.push_back(uint64_t(current[i]->feedback_copy->image));}
}
void copy(Image &source,VkCommandBuffer cmd) {
    spatial::LiteTraceScope scope("NVN.Feedback.Snapshot");
    if(!source.feedback_copy){
        auto p=std::make_shared<Image>();p->source=source.source;p->fmt=source.fmt;p->aspects=source.aspects;
        p->width=source.width;p->height=source.height;p->depth=source.depth;p->levels=source.levels;p->layers=source.layers;
        p->bpp=source.bpp;p->samples=source.samples;p->uploaded=true;p->mng_set_gpu_written(true);
        auto ci=source.create;ci.pNext=nullptr;ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        ci.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.flags|=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;p->mutable_format=true;p->usage=ci.usage;p->create=ci;
        VK(vkCreateImage(cfg.device,&ci,nullptr,&p->image));VkMemoryRequirements r;vkGetImageMemoryRequirements(cfg.device,p->image,&r);
        p->mem_bytes=int64_t(r.size);image_bytes_live+=p->mem_bytes;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=r.size;
        ai.memoryTypeIndex=memory_type(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK(vkAllocateMemory(cfg.device,&ai,nullptr,&p->memory));VK(vkBindImageMemory(cfg.device,p->image,p->memory,0));
        source.feedback_copy=std::move(p);
    }
    auto &dest=*source.feedback_copy;
    end_open_pass(11);full_barrier(cmd);
    source.transition(cmd,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);dest.transition(cmd,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    std::vector<VkImageCopy> regions;regions.reserve(source.levels);
    for(unsigned level=0;level<source.levels;++level){VkImageCopy region{};
        region.srcSubresource=region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,level,0,source.layers};
        region.extent={std::max(1u,source.width>>level),std::max(1u,source.height>>level),1};regions.push_back(region);}
    {mng::profile::GpuCommandScope gpu_scope(cmd,"GPU.FeedbackCopy",false);
        vkCmdCopyImage(cmd,source.image,source.layout,dest.image,dest.layout,uint32_t(regions.size()),regions.data());}
    source.transition(cmd,VK_IMAGE_LAYOUT_GENERAL);dest.transition(cmd,VK_IMAGE_LAYOUT_GENERAL);full_barrier(cmd);
    source.feedback_epoch=epoch;++copies;spatial::LiteTrace::counter("NVN.Feedback.Copies",copies);
}
uint64_t target_generation=1,target_cached=0,target_images=0;
std::array<Image*,8> targets{};
void targets_changed(){++target_generation;boundary();}
struct MatchCache {
    std::array<uint64_t,24> key{};
    std::array<const tex_t*,8> textures{};
    unsigned count=0;uint32_t sampled=0;bool valid=false;
} match;
void begin_draw() {
    current_count=0;sampled_attachments=0;sampled_attachments_known=false;
    if(!enabled() || state.raster_discard)return;
    std::array<uint64_t,24> signature{};unsigned at=0;
    for(unsigned st=0;st<5;++st)signature[at++]=uint64_t(uintptr_t(state.shader[st]));
    for(unsigned st=0;st<6;++st)signature[at++]=tex_gen[st];
    signature[at++]=target_generation;signature[at++]=images_epoch;signature[at++]=memo_check_gen;
    signature[at++]=uint64_t(uintptr_t(state.texture_pool.pool));signature[at++]=state.texture_pool.off;
    signature[at++]=state.texture_pool.n;signature[at++]=state.mask.mask;
    if(!match.valid || match.key!=signature){
        match.valid=false;match.count=0;match.sampled=0;
        bool samples_textures=false;
        for(unsigned st=0;st<5;++st)if(auto *shader=state.shader[st]){
            if(!shader->feedback_learned){
                for(const auto &b:shader->bindings){
                    shader->feedback_storage|=b.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    if(b.source!=XBG_SOURCE_TEXTURE || (b.type!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b.type!=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE))continue;
                    need(b.slot+b.count<=128,"Feedback texture slots");
                    for(unsigned i=0;i<b.count;++i)shader->feedback_slots.push_back(uint16_t(b.slot+i));
                }
                shader->feedback_learned=true;
            }
            if(shader->feedback_storage){boundary();return;}
            samples_textures|=!shader->feedback_slots.empty();
        }
        if(!samples_textures){match.key=signature;match.valid=true;sampled_attachments_known=true;boundary();return;}
        if(target_cached!=target_generation || target_images!=images_epoch){
            targets={};for(int i=0;i<state.rt.n;++i)if(state.rt.color[i]){
                auto it=images.find(state.rt.color[i]->serial);
                targets[i]=it!=images.end()?it->second.get():&get_image(state.rt.color[i],false);
            }
            target_cached=target_generation;target_images=images_epoch;
        }
        auto pool=textures.find(pool_key(&state.texture_pool));const auto &table=pool==textures.end()?legacy_textures:pool->second;
        for(unsigned st=0;st<5;++st)if(state.shader[st])for(unsigned slot:state.shader[st]->feedback_slots){
            const int id=int(state.tex[nvn_stage[st]][slot]&0xfffff);
            if(!id || (state.texture_pool.n>0 && id>=state.texture_pool.n))continue;
            auto t=table.find(id);need(t!=table.end(),"Unregistered feedback texture");const auto &texture=t->second.texture;
            const auto *depth=state.rt.depth;
            if(depth && texture.mem==depth->mem && !memcmp(&texture.d,&depth->d,sizeof texture.d))match.sampled|=1u<<8;
            // get_image's physical aliases require identical backing/layout.
            // Compare those facts directly instead of looking up every sampled
            // image in the global image map on every draw.
            for(unsigned index=0;index<targets.size();++index){auto *target=targets[index];
                if(!target || texture.mem!=target->source.mem || memcmp(&texture.d,&target->source.d,sizeof(texture.d)))continue;
                match.sampled|=1u<<index;
                if(!((state.mask.mask>>(4*index))&15))continue;
                if(target->aspects!=VK_IMAGE_ASPECT_COLOR_BIT || target->samples!=VK_SAMPLE_COUNT_1_BIT || target->create.imageType!=VK_IMAGE_TYPE_2D || target->decoded){boundary();return;}
                bool found=false;for(unsigned i=0;i<match.count;++i)found|=match.textures[i]->serial==texture.serial;
                if(!found){need(match.count<match.textures.size(),"Feedback attachment count");match.textures[match.count++]=&texture;}
            }
        }
        match.key=signature;match.valid=true;
    }
    sampled_attachments=match.sampled;sampled_attachments_known=true;
    for(unsigned i=0;i<match.count;++i){auto &source=get_image(match.textures[i],true);
        bool found=false;for(unsigned k=0;k<current_count;++k)found|=current[k]==&source;
        if(!found)current[current_count++]=&source;
    }
    if(!current_count){boundary();return;}
    VkCommandBuffer cmd=recording_slot().cmd;
    for(unsigned i=0;i<current_count;++i)if(current[i]->feedback_epoch!=epoch)copy(*current[i],cmd);
    ++draws;
}
}
extern "C" uint64_t mng_nvn_feedback_copies(){return mng_feedback::copies;}
