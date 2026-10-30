// SPDX-License-Identifier: MIT
// Included after descriptor construction and before draw(). Translation owns
// the renderer mutex; none of these checks acquires a slot or a GPU resource.
namespace mng_reads {
bool &buffer_filter(){static bool on=[] {const char *v=getenv("MNG_PREFLIGHT_BUFFER_FILTER");return !v || strcmp(v,"0");}();return on;}
bool enabled() {
    static const bool value=[] {const char *v=getenv("MNG_RANGE_PREFLIGHT");return !v || strcmp(v,"0");}();
    return value;
}
bool gpu_view(uint64_t address,uint64_t bytes,VkDeviceSize alignment) {
    // Match pending_view's newest-overlap rule, without pinning a slot. A
    // partial newer write must not be hidden by an older, fully covering one.
    for(auto w=pending_writebacks.rbegin();w!=pending_writebacks.rend();++w){
        if(!xbg_wb_overlaps(address,bytes,w->addr,w->bytes))continue;
        return address>=w->addr && address+bytes<=w->addr+w->bytes &&
            (alignment<=1 || (w->buffer.offset+address-w->addr)%alignment==0);
    }
    return false;
}
void range(uint64_t address,uint64_t bytes,unsigned why,VkDeviceSize alignment=0) {
    if(!address || !bytes)return; // The consuming operation validates bindings.
    need(xbg_wb_range_valid(address,bytes),"Preflight range overflow");
    if(alignment && gpu_view(address,bytes,alignment))return;
    const auto before=pending_writebacks.size()+pending_reports.size();
    cpu_access(address,bytes,why);
    if(before && pending_writebacks.empty() && pending_reports.empty())++resolved;
}
void texture(const tex_t *t,bool upload) {
    if(t && t->mem && (upload || images.find(t->serial)==images.end()))
        range(uint64_t(t->mem),t->L.size,9);
}
void attachments() {
    if(!read_preflight)return;
    if(!enabled()){preflight_cpu_reads();return;}
    if(pending_writebacks.empty() && pending_reports.empty())return;
    spatial::LiteTraceScope scope("NVN.CpuRead.Attachments");
    need(state.rt.n>=0 && state.rt.n<=8,"Preflight target count out of range");
    // A partial clear may import the target's original CPU contents. A folded
    // clear can create a pass containing every bound attachment.
    for(int i=0;i<state.rt.n;++i)texture(state.rt.color[i],true);
    texture(state.rt.depth,true);
}
void images_read(const tex_t *a,const tex_t *b=nullptr) {
    if(!read_preflight)return;
    if(!enabled()){preflight_cpu_reads();return;}
    if(pending_writebacks.empty() && pending_reports.empty())return;
    spatial::LiteTraceScope scope("NVN.CpuRead.Images");
    texture(a,true);texture(b,true);
}
void operation(bool compute,const p_draw_idx *indexed=nullptr,int count=0) {
    if(!read_preflight)return;
    if(!enabled()){preflight_cpu_reads();return;}
    if(pending_writebacks.empty() && pending_reports.empty())return;
    spatial::LiteTraceScope scope("NVN.CpuRead.Preflight");
    ++checked;
    // Only the range shapes are cached. Pending GPU results are checked below
    // on every operation, through the unchanged range/cpu_access path.
    using Range=mng::vulkan::preparation::StorageRange;
    static std::vector<Range> ranges;ranges.clear();
    const auto *prepared_ranges=mng_prepare::storage_ranges(compute);
    mng_prepare::elements(compute,[&](Shader &shader,unsigned st,unsigned ns,const xbg_shader_binding &b,unsigned i,bool written){
        if(buffer_filter() && pending_writebacks.empty() && b.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER &&
           (b.source==XBG_SOURCE_UNIFORM || b.source==XBG_SOURCE_STORAGE))return true;
        const unsigned slot=b.slot+i;
        if(b.source==XBG_SOURCE_STORAGE || b.source==XBG_SOURCE_UNIFORM){
            need(slot<16,"Preflight buffer slot out of range");
            const auto r=b.source==XBG_SOURCE_STORAGE?state.sb[ns][slot]:state.ub[ns][slot];
            if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER){
                if(!prepared_ranges){
                    if(!xbg_wb_range_valid(r.address,r.size))xbg_fatalf("Invalid preflight storage range shader=%s stage=%u slot=%u address=%#llx bytes=%llu",shader.key.c_str(),st,slot,(unsigned long long)r.address,(unsigned long long)r.size);
                    ranges.push_back({r.address,r.address+r.size,written});
                }
            }else if(b.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER){
                const auto bytes=std::min<uint64_t>(r.size,b.bytes?b.bytes:65536);
                if(!gpu_view(r.address,uniform_range(bytes),properties.limits.minUniformBufferOffsetAlignment))range(r.address,bytes,2);
            }
        }else if(b.source==XBG_SOURCE_TEXTURE || b.source==XBG_SOURCE_IMAGE){
            need(slot<(b.source==XBG_SOURCE_IMAGE?16u:128u),"Preflight image slot out of range");
            const auto handle=b.source==XBG_SOURCE_IMAGE?state.img[ns][slot]:state.tex[ns][slot];
            const int id=int(handle&0xfffff);
            if(id==0 || (state.texture_pool.n>0 && id>=state.texture_pool.n))return true;
            auto pool=textures.find(pool_key(&state.texture_pool));
            const auto &table=pool==textures.end()?legacy_textures:pool->second;
            auto found=table.find(id);need(found!=table.end(),"Unregistered preflight texture");
            const tex_t t=found->second.texture;
            if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER)range(uint64_t(t.mem),t.L.size,8,1);
            else if(b.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)range(uint64_t(t.mem),t.L.size,1);
            else texture(&t,true);
        }
        return true;
    });
    if(!prepared_ranges){mng::vulkan::preparation::merge_ranges(ranges);prepared_ranges=&ranges;}
    for(const auto &merged:*prepared_ranges)
        range(merged.begin,merged.end-merged.begin,merged.written?3:1,
              merged.written?1:properties.limits.minStorageBufferOffsetAlignment);
    if(!compute){
        if((!buffer_filter() || !pending_writebacks.empty()) && state.shader[0])for(auto loc:state.shader[0]->inputs){
            need(loc<16,"Preflight vertex location out of range");va_st attr;memcpy(&attr,&state.attrib[loc],sizeof(attr));
            if(attr.fmt){need(attr.stream<16,"Preflight vertex stream out of range");auto r=state.vb[attr.stream];range(r.address,r.size,1,4);}
        }
        // Eight-bit indices are converted on the CPU (reason=5), so they must
        // still check counter reports. Native indices use reason=1.
        if(indexed && count>0 && (!buffer_filter() || !pending_writebacks.empty() || !indexed->itype)){need(indexed->itype>=0 && indexed->itype<=2,"Preflight index type out of range");
            range(indexed->ibuf,uint64_t(count)*(1u<<indexed->itype),indexed->itype?1:5,indexed->itype?4:0);}
        if(g_ui_rect_fn){auto v=state.vb[0],u=state.ub[0][0];range(v.address,v.size,5);range(u.address,u.size,5);}
        need(state.rt.n>=0 && state.rt.n<=8,"Preflight target count out of range");
        for(int i=0;i<state.rt.n;++i)texture(state.rt.color[i],false);
        texture(state.rt.depth,false);
    }
}
struct Guard {
    Guard(){need(!active,"Nested draw/dispatch translation");active=true;}
    ~Guard(){active=false;}
};
}
extern "C" uint64_t mng_nvn_range_preflight_resolves(){return mng_reads::resolved;}

uint64_t mng_batch_submissions=0;
uint64_t mng_batch_deferred=0;
extern "C" uint64_t mng_nvn_batch_submissions(){return mng_batch_submissions;}
extern "C" uint64_t mng_nvn_batch_deferred(){return mng_batch_deferred;}
namespace mng_batch {
unsigned &limit(){
    static unsigned value=[] {
        const char *value=getenv("MNG_SUBMIT_BATCH_OPS");
        if(!value)return 1024u;
        char *end=nullptr;const unsigned long parsed=strtoul(value,&end,10);
        need(end && !*end && parsed<=65536,"Invalid GPU submission batch limit");
        return unsigned(parsed);
    }();return value;
}
bool &adaptive(){
    static bool value=[] {const char *v=getenv("MNG_ADAPTIVE_BATCH");return !v || strcmp(v,"0");}();
    return value;
}
}
// Changes are serialized at an engine boundary. A diagnostic request never
// waits for translation, GPU completion, or a Guest core.
extern "C" int mng_nvn_ordered_status(char*,unsigned);
extern "C" void mng_nvn_live_flush_mode(int);
extern "C" int mng_nvn_live_flush_status(char*,unsigned);
extern "C" void mng_nvn_range_invalidate_mode(int);
extern "C" int mng_nvn_schedule(int adaptive,int limit,int present,int invalidate,int reads,int descriptors,int images_fast,int pipeline,int plans,int flush,char *out,unsigned bytes){
    std::unique_lock lock(mutex,std::try_to_lock);if(!lock.owns_lock())return -1;
    if(!initialized)return snprintf(out,bytes,"error=renderer_not_initialized\nretryable=1\n");
    if(flush>=0)mng_nvn_live_flush_mode(flush);
    if(adaptive>=0)mng_batch::adaptive()=adaptive!=0;
    if(limit>=0)mng_batch::limit()=unsigned(limit);
    if(present>=0)mng_present_transition=present!=0;
    if(invalidate>=0){mng_range_invalidate=invalidate!=0;mng_nvn_range_invalidate_mode(invalidate);}
    if(reads>=0)mng_reads::buffer_filter()=reads!=0;
    if(descriptors>=0)mng_translate::descriptor_plan()=descriptors!=0;
    if(images_fast>=0)mng_translate::image_fast()=images_fast!=0;
    if(plans>=0 && mng_translate::preparation_plan()!=(plans!=0)){mng_translate::preparation_plan()=plans!=0;mng_prepare::invalidate();}
    if(pipeline>=0)mng_translate::pipeline_early()=pipeline!=0;
    const auto jobs=warm::job_snapshot();
    const int used=snprintf(out,bytes,"schema=mng.renderer-schedule.v1\npolicy=shared_nvn_vulkan_v1\nadaptive=%u\nbatch_ops=%u\nbatch_submissions=%llu\nbatch_deferred=%llu\npresent_transition=%u\npresent_capable=%u\nrange_invalidate=%u\npreflight_buffer_filter=%u\nfeedback_snapshot=%u\ndefer_fragment_barrier=%u\npipeline_memo=%u\nperf_sample=%u\nread_preflight=%u\nrange_preflight=%u\nread_range_cache=%u\npresent_worker=%s\ndescriptor_plan=%u\nimage_read_fast=%u\nupload_cached=%u\npipeline_early=%u\npipeline_early_hits=%llu\npipeline_state_builds=%llu\npso_workers=%u\npso_submitted=%llu\npso_built=%llu\npso_failed=%llu\npso_active=%llu\npso_peak=%llu\npso_joined=%llu\npso_claimed=%llu\n",
        unsigned(mng_batch::adaptive()),mng_batch::limit(),
        (unsigned long long)mng_batch_submissions,(unsigned long long)mng_batch_deferred,
        unsigned(mng_present_transition),unsigned(mng_present_source_commands),unsigned(mng_range_invalidate),unsigned(mng_reads::buffer_filter()),
        unsigned(mng_feedback::enabled()),unsigned(mng_fragment_barrier::enabled()),unsigned(pipe_memo),perf_sample,
        unsigned(read_preflight),unsigned(mng_reads::enabled()),unsigned(mng_read_cache::enabled()),getenv("MNG_PRESENT_WORKER"),unsigned(mng_translate::descriptor_plan()),unsigned(mng_translate::image_fast()),unsigned(mng::vulkan::policy::upload_cached()),unsigned(mng_translate::pipeline_early()),(unsigned long long)mng_translate::pipeline_early_hits,(unsigned long long)mng_translate::pipeline_state_builds,
        warm::job_workers,(unsigned long long)jobs.submitted,(unsigned long long)jobs.built,(unsigned long long)jobs.failed,
        (unsigned long long)jobs.active,(unsigned long long)jobs.peak,(unsigned long long)jobs.waits,(unsigned long long)jobs.claimed);
    if(used<0 || unsigned(used)>=bytes)return used;
    const auto &p=mng_prepare::counts;
    const int details=snprintf(out+used,bytes-unsigned(used),"preparation_plan=%u\npreparation_validate=%u\npreparation_shader_builds=%llu\npreparation_binding_builds=%llu\npreparation_binding_reuses=%llu\npreparation_key_builds=%llu\npreparation_key_reuses=%llu\npreparation_range_builds=%llu\npreparation_range_reuses=%llu\npreparation_pipeline_reuses=%llu\npreparation_fallbacks=%llu\npreparation_metadata_fallbacks=%llu\npreparation_legacy_key_fallbacks=%llu\npreparation_key_ineligible=%llu\npreparation_metadata_bytes=%zu\n",
        unsigned(mng_translate::preparation_plan()),unsigned(mng_prepare::validate()),
        (unsigned long long)p.shader_builds,(unsigned long long)p.binding_builds,(unsigned long long)p.binding_reuses,
        (unsigned long long)p.key_builds,(unsigned long long)p.key_reuses,(unsigned long long)p.range_builds,
        (unsigned long long)p.range_reuses,(unsigned long long)p.pipeline_reuses,(unsigned long long)p.fallbacks,
        (unsigned long long)p.metadata_fallbacks,(unsigned long long)p.legacy_key_fallbacks,
        (unsigned long long)p.key_ineligible,mng_prepare::metadata_bytes);
    if(details<0 || unsigned(details)>=bytes-unsigned(used))return details<0?details:used+details;
    const int f=mng_nvn_live_flush_status(out+used+details,bytes-unsigned(used+details));
    if(f<0 || unsigned(f)>=bytes-unsigned(used+details))return f<0?f:used+details+f;
    return used+details+f+mng_nvn_ordered_status(out+used+details+f,bytes-unsigned(used+details+f));
}
void mng_submit_batch(unsigned next_op) {
    const unsigned limit=mng_batch::limit();
    // Submit only between fully translated operations, at an explicit target
    // binding boundary. Never split a draw to hit the fixed count.
    // Keep the two-slot ownership model. Do not finish the current batch just
    // to block on the next arena: its GPU owner can finish while we translate
    // more commands into this arena. Arena limits, explicit flushes and Guest
    // synchronization still submit unconditionally through their own paths.
    if(limit && next_op==OP_SET_RT && slots[slot_index].recording && slots[slot_index].ops>=limit){
        need(!mng_reads::active,"GPU batch split inside resource translation");
        Slot &next=slots[slot_index^1];
        if(mng_batch::adaptive() && next.pending){
            const VkResult ready=vkGetFenceStatus(cfg.device,next.fence);
            if(ready==VK_NOT_READY){++mng_batch_deferred;return;}
            VK(ready);
        }
        spatial::LiteTraceScope scope("NVN.Batch.Submit");
        submit_slots(false);
        ++mng_batch_submissions;
    }
}

extern "C" int mng_engine_publication_counters(uint64_t* epoch,uint64_t* rebuilds){
    std::unique_lock lock(mutex,std::try_to_lock);if(!lock.owns_lock())return -1;
    *epoch=images_epoch;*rebuilds=mng_image_index_rebuilds;return 0;
}
