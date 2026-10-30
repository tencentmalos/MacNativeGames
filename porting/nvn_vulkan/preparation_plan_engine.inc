// SPDX-License-Identifier: MIT
// Included after Shader and State. Translation owns every plan and revision.
namespace mng_prepare {
using namespace mng::vulkan::preparation;
Versions versions;
struct Counts {
    uint64_t shader_builds{}, binding_builds{}, binding_reuses{}, key_builds{}, key_reuses{};
    uint64_t range_builds{}, range_reuses{}, pipeline_reuses{}, fallbacks{}, metadata_fallbacks{}, legacy_key_fallbacks{}, key_ineligible{};
} counts;
size_t metadata_bytes=0;
constexpr size_t metadata_budget=16u<<20;
bool enabled() {
    return mng_translate::preparation_plan() && !capture.active && !provenance_file && g_ui_moff==0;
}
bool validate() {
    static const bool value=[] {const char *v=getenv("MNG_PREPARATION_VALIDATE");return v && !strcmp(v,"1");}();
    return value;
}
struct Group { Shader *shader; unsigned stage,ns; xbg_shader_binding binding; bool written; };
struct Element { uint32_t group,element; };
struct KeyCache {
    RevisionWatch watch;
    std::vector<uint64_t> words;
    std::vector<uint8_t> tags;
    bool ready{},ok{},texel_enabled{};
    uint64_t epoch{};
    void clear(){watch={};words.clear();tags.clear();ready=false;}
};
struct BindingSet {
    std::array<Shader*,6> shaders{};
    bool ready{},valid{};
    unsigned uniforms{},varying_uniforms{};
    std::vector<Group> groups;
    std::vector<Element> elements;
    KeyCache keys[3];
    RevisionWatch range_watch;
    std::vector<StorageRange> ranges;
    void clear(){ready=valid=false;groups.clear();elements.clear();for(auto &key:keys)key.clear();range_watch={};ranges.clear();}
} binding_sets[2];
void invalidate() {
    for(auto &plan:binding_sets)plan.clear();
    bump(versions.fixed);bump(versions.vertex);bump(versions.blend);
}
void reset(){invalidate();counts={};metadata_bytes=0;}
BindingSet *bindings(bool compute) {
    if(!enabled())return nullptr;
    std::array<Shader*,6> selected{};
    for(unsigned st=compute?5:0;st<(compute?6:5);++st)selected[st]=state.shader[st];
    auto &plan=binding_sets[unsigned(compute)];
    if(plan.ready && plan.shaders==selected){++counts.binding_reuses;return plan.valid?&plan:nullptr;}
    plan.clear();plan.ready=true;plan.shaders=selected;plan.uniforms=plan.varying_uniforms=0;
    for(unsigned st=compute?5:0;st<(compute?6:5);++st)if(auto *shader=selected[st]){
        auto &metadata=shader->preparation;
        if(!metadata.ready){
            size_t required=shader->bindings.size()*sizeof(decltype(metadata.groups)::value_type);
            for(const auto &b:shader->bindings)required+=size_t(b.count)*sizeof(decltype(metadata.elements)::value_type);
            if(required>metadata_budget-metadata_bytes){metadata.ready=true;++counts.fallbacks;++counts.metadata_fallbacks;return nullptr;}
            build(metadata,shader->bindings,
                [&](const auto &b){return b.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && shader->storage_writes.count(b.binding)!=0;},
                [](const auto &b){return b.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;},
                [](const auto &b){return b.source==XBG_SOURCE_UNIFORM || b.source==XBG_SOURCE_DRIVER0 || b.source==XBG_SOURCE_DRIVER2;});
            if(metadata.valid)metadata_bytes+=required;
            ++counts.shader_builds;
        }
        if(!metadata.valid || plan.elements.size()+metadata.elements.size()>4096){++counts.fallbacks;++counts.metadata_fallbacks;return nullptr;}
        const auto first=uint32_t(plan.groups.size());
        for(const auto &group:metadata.groups){
            if(group.binding.type>VK_DESCRIPTOR_TYPE_STORAGE_BUFFER){++counts.fallbacks;++counts.metadata_fallbacks;return nullptr;}
            plan.groups.push_back({shader,st,nvn_stage[st],group.binding,group.written});
        }
        for(auto element:metadata.elements)plan.elements.push_back({first+element.group,element.element});
        plan.uniforms+=metadata.uniforms;plan.varying_uniforms+=metadata.varying_uniforms;
    }
    for(const auto &group:plan.groups){const auto &b=group.binding;const auto ns=group.ns;
        for(unsigned i=0;i<b.count;++i){const unsigned slot=b.slot+i;
            if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && (b.source==XBG_SOURCE_STORAGE || b.source==XBG_SOURCE_UNIFORM) && slot<16){
                const auto &v=b.source==XBG_SOURCE_STORAGE?versions.storage[ns][slot]:versions.uniform[ns][slot];
                plan.range_watch.add(v.address);plan.range_watch.add(v.shape);
            }
            for(unsigned mode=0;mode<3;++mode){auto &watch=plan.keys[mode].watch;
                switch(b.source){
                case XBG_SOURCE_UNIFORM:case XBG_SOURCE_STORAGE:
                    if(slot<16){const auto &v=b.source==XBG_SOURCE_UNIFORM?versions.uniform[ns][slot]:versions.storage[ns][slot];
                        const bool dynamic=b.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b.source==XBG_SOURCE_UNIFORM && mode!=0;
                        if(!dynamic)watch.add(v.address);watch.add(v.shape);}
                    break;
                case XBG_SOURCE_SUPPORT:watch.add(versions.support);break;
                case XBG_SOURCE_DRIVER0:if(!(mode && b.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER))watch.add(tex_gen[ns]);break;
                case XBG_SOURCE_DRIVER2:if(!(mode && b.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER))watch.add(buf_gen[ns]);break;
                case XBG_SOURCE_TEXTURE:watch.add(versions.textures[ns]);if(b.sampler_slot!=UINT32_MAX)watch.add(versions.samplers[ns]);break;
                case XBG_SOURCE_IMAGE:watch.add(versions.images[ns]);if(b.sampler_slot!=UINT32_MAX)watch.add(versions.samplers[ns]);break;
                default:break;
                }
            }
        }
    }
    plan.valid=true;++counts.binding_builds;return &plan;
}
template<class Function>
bool groups(bool compute,Function function) {
    if(auto *plan=bindings(compute)){
        for(const auto &group:plan->groups)if(!function(*group.shader,group.stage,group.ns,group.binding,group.written))return false;
    }else for(unsigned st=compute?5:0;st<(compute?6:5);++st)if(auto *shader=state.shader[st])
        for(const auto &b:shader->bindings)if(!function(*shader,st,nvn_stage[st],b,b.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && shader->storage_writes.count(b.binding)!=0))return false;
    return true;
}
template<class Function>
bool elements(bool compute,Function function) {
    if(auto *plan=bindings(compute)){
        for(const auto element:plan->elements){const auto &g=plan->groups[element.group];
            if(!function(*g.shader,g.stage,g.ns,g.binding,element.element,g.written))return false;}
    }else for(unsigned st=compute?5:0;st<(compute?6:5);++st)if(auto *shader=state.shader[st])
        for(const auto &b:shader->bindings){need(b.count && b.count<=128,"Unsupported descriptor array count");
            const bool written=b.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && shader->storage_writes.count(b.binding)!=0;
            for(unsigned i=0;i<b.count;++i)if(!function(*shader,st,nvn_stage[st],b,i,written))return false;}
    return true;
}
size_t element_count(bool compute) {
    if(auto *plan=bindings(compute))return plan->elements.size();
    size_t count=0;for(unsigned st=compute?5:0;st<(compute?6:5);++st)if(state.shader[st])for(const auto &b:state.shader[st]->bindings)count+=b.count;
    return count;
}
const std::vector<StorageRange> *storage_ranges(bool compute) {
    auto *plan=bindings(compute);if(!plan)return nullptr;
    if(plan->range_watch.unchanged()){++counts.range_reuses;return &plan->ranges;}
    plan->ranges.clear();
    for(const auto element:plan->elements){const auto &group=plan->groups[element.group];const auto &b=group.binding;
        if(b.type!=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || (b.source!=XBG_SOURCE_STORAGE && b.source!=XBG_SOURCE_UNIFORM))continue;
        const unsigned slot=b.slot+element.element;if(slot>=16)return nullptr;
        const auto r=b.source==XBG_SOURCE_STORAGE?state.sb[group.ns][slot]:state.ub[group.ns][slot];
        if(!r.address || !r.size || r.size>properties.limits.maxStorageBufferRange || r.address+r.size<r.address)return nullptr;
        plan->ranges.push_back({r.address,r.address+r.size,group.written});
    }
    merge_ranges(plan->ranges);plan->range_watch.remember();++counts.range_builds;return &plan->ranges;
}
void counters() {
    spatial::LiteTrace::counter("NVN.Preparation.BindingBuilds",counts.binding_builds);
    spatial::LiteTrace::counter("NVN.Preparation.KeyBuilds",counts.key_builds);
    spatial::LiteTrace::counter("NVN.Preparation.KeyReuses",counts.key_reuses);
    spatial::LiteTrace::counter("NVN.Preparation.RangeBuilds",counts.range_builds);
    spatial::LiteTrace::counter("NVN.Preparation.RangeReuses",counts.range_reuses);
    spatial::LiteTrace::counter("NVN.Preparation.PipelineReuses",counts.pipeline_reuses);
}
}
