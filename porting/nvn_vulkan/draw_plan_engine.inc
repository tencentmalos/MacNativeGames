// SPDX-License-Identifier: MIT
// Exact state caches. Only host structure is versioned; Guest buffers still
// pass through the existing snapshots and dependency checks on every draw.
namespace mng_draw_plan {
bool enabled(){static const bool on=[] {const char *v=getenv("MNG_DRAW_PLAN");return !v || strcmp(v,"0");}();return on;}
struct Vertex {
    Shader *shader{};
    uint32_t attrib[16]{};
    vs_st stream[16]{};
    uint64_t revision{};
    bool valid=false;
    int zero_binding=-1;
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<VkVertexInputBindingDescription> bindings;
    bool matches()const{
        if(!enabled() || capture.active || !valid || shader!=state.shader[0])return false;
        if(mng_prepare::enabled()){
            const bool same=revision==mng_prepare::versions.vertex;
            if(same && mng_prepare::validate())need(!memcmp(attrib,state.attrib,sizeof attrib) && !memcmp(stream,state.stream,sizeof stream),"Preparation vertex revision missed a state change");
            return same;
        }
        return !memcmp(attrib,state.attrib,sizeof attrib) && !memcmp(stream,state.stream,sizeof stream);
    }
    void remember(){shader=state.shader[0];memcpy(attrib,state.attrib,sizeof attrib);memcpy(stream,state.stream,sizeof stream);revision=mng_prepare::versions.vertex;valid=true;}
} vertex;
struct Blend {
    blend_st blend[8]{};
    uint32_t enabled_mask{},write_mask{};
    uint64_t revision{};
    int count=-1;
    bool valid=false;
    std::vector<VkPipelineColorBlendAttachmentState> attachments;
    bool matches()const{
        if(!enabled() || !valid || count!=state.rt.n)return false;
        if(mng_prepare::enabled()){
            const bool same=revision==mng_prepare::versions.blend;
            if(same && mng_prepare::validate())need(enabled_mask==state.color.blend_en && write_mask==state.mask.mask && !memcmp(blend,state.blend,sizeof blend),"Preparation blend revision missed a state change");
            return same;
        }
        return enabled_mask==state.color.blend_en && write_mask==state.mask.mask && !memcmp(blend,state.blend,sizeof blend);
    }
    void remember(){count=state.rt.n;enabled_mask=state.color.blend_en;write_mask=state.mask.mask;memcpy(blend,state.blend,sizeof blend);revision=mng_prepare::versions.blend;valid=true;}
} blend;
struct Pipeline {
    std::array<Shader*,5> shaders{};
    const void *layout{},*definition{};
    VkRenderPass pass{};
    VkPipelineLayout pipeline_layout{};
    uint64_t revision{};
    int primitive{};
    uint32_t samples{},targets{},colors{},depth_format{};
    bool depth{},valid{};
    bool matches(const void *l,const void *d,VkRenderPass p,VkPipelineLayout pl,int prim,VkSampleCountFlagBits s,uint32_t c)const{
        if(!mng_prepare::enabled() || !valid || !pipe_memo_valid || revision!=mng_prepare::versions.fixed ||
           layout!=l || definition!=d || pass!=p || pipeline_layout!=pl || primitive!=prim || samples!=uint32_t(s) ||
           targets!=uint32_t(state.rt.n) || colors!=c || depth!=(state.rt.depth!=nullptr) ||
           depth_format!=(state.rt.depth?uint32_t(state.rt.depth->d.format):0))return false;
        for(unsigned i=0;i<5;++i)if(shaders[i]!=state.shader[i])return false;
        return true;
    }
    void remember(const void *l,const void *d,VkRenderPass p,VkPipelineLayout pl,int prim,VkSampleCountFlagBits s,uint32_t c){
        if(!mng_prepare::enabled()){valid=false;return;}
        for(unsigned i=0;i<5;++i)shaders[i]=state.shader[i];
        layout=l;definition=d;pass=p;pipeline_layout=pl;primitive=prim;samples=uint32_t(s);
        targets=uint32_t(state.rt.n);colors=c;depth=state.rt.depth!=nullptr;
        depth_format=state.rt.depth?uint32_t(state.rt.depth->d.format):0;
        revision=mng_prepare::versions.fixed;valid=true;
    }
} pipeline;
void reset(){vertex=Vertex{};blend=Blend{};pipeline=Pipeline{};}
}
