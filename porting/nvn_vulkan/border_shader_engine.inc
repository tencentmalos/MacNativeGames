// SPDX-License-Identifier: MIT
// Included after Shader, State and load_shader_key, inside the engine namespace.
constexpr uint32_t MNG_SOURCE_BORDER=0x4D4E47;
bool border_emulation_enabled(){
    static const bool forced=getenv("MNG_EMULATE_BORDER") && strcmp(getenv("MNG_EMULATE_BORDER"),"0");
    return forced || !mng_vk_custom_border_enabled();
}
std::map<std::string,std::unique_ptr<Shader>> border_variants;
Shader* border_variant(Shader* base){
    if(!border_emulation_enabled())return base;
    spatial::LiteTraceScope scope("NVN.Shader.BorderVariant");
    std::vector<mng::shader::BorderBinding> requests;
    std::string key=base->key;
    uint32_t next_binding=base->stage;
    for(const auto& b:base->bindings)next_binding=std::max(next_binding,(b.binding/6+1)*6+base->stage);
    const unsigned ns=nvn_stage[base->stage];
    for(const auto& b:base->bindings){
        if(b.type!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)continue;
        need(b.source==XBG_SOURCE_TEXTURE && b.slot<128,"Unsupported border texture source");
        need(b.sampler_slot==UINT32_MAX || b.sampler_slot<128,"Border sampler slot out of range");
        const uint64_t handle=b.sampler_slot==UINT32_MAX?state.tex[ns][b.slot]:state.samp[ns][b.sampler_slot];
        const int sid=int(handle>>20);if(!sid)continue;
        auto pool=samplers.find(pool_key(&state.sampler_pool));auto& table=pool==samplers.end()?legacy_samplers:pool->second;
        auto found=table.find(sid);need(found!=table.end(),"Border sampler descriptor missing");
        const auto& p=found->second;
        if(p.ws!=4 && p.ws!=5 && p.wt!=4 && p.wt!=5 && p.wr!=4 && p.wr!=5)continue;
        const bool black=p.border[0]==0 && p.border[1]==0 && p.border[2]==0 && (p.border[3]==0 || p.border[3]==1);
        const bool white=p.border[0]==1 && p.border[1]==1 && p.border[2]==1 && p.border[3]==1;
        if(black || white)continue;
        need(b.count==1 && !p.cmp_mode && !((b.flags>>16)&3),"Custom border fallback requires scalar non-comparison float sampler");
        mng::shader::BorderBinding request{b.binding,next_binding};next_binding+=6;
        requests.push_back(request);
        key+=':';key+=std::to_string(b.binding);
    }
    if(requests.empty())return base;
    if(auto found=border_variants.find(key);found!=border_variants.end())return found->second.get();
    need(border_variants.size()<4096,"Custom border shader variant budget exceeded");
    const auto start=timing_clock();std::vector<uint32_t> words;
    try{words=mng::shader::emulate_border(base->spirv,requests,next_binding);}
    catch(const std::exception& error){xbg_fatalf("Custom border shader %s: %s",base->key.c_str(),error.what());}
    auto variant=std::make_unique<Shader>(*base);variant->module=VK_NULL_HANDLE;variant->key=key;
    variant->preparation={};variant->uniform_counts_known=false;variant->uniform_all=variant->uniform_vary=0;
    for(const auto& request:requests){
        auto found=std::find_if(base->bindings.begin(),base->bindings.end(),[&](const auto& b){return b.binding==request.binding;});
        variant->border_sources.push_back(*found);
        auto white=*found;white.binding=request.white_binding;variant->bindings.push_back(white);
        variant->border_canonical[request.binding]=0;variant->border_canonical[request.white_binding]=1;
    }
    variant->bindings.push_back({next_binding,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,uint32_t(requests.size()*16),MNG_SOURCE_BORDER,0,0,0});
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=words.size()*4;ci.pCode=words.data();
    VK(vkCreateShaderModule(cfg.device,&ci,nullptr,&variant->module));
    // Runtime variants deliberately stay out of the XGS-only warm-up list.
    // The VkPipelineCache still caches their compiled driver representation.
    auto* result=variant.get();border_variants.emplace(key,std::move(variant));
    ++stats.shader_loads;++hud::shaders;hud::shader_ns+=timing_clock()-start;
    xbg_logf("NVN border fallback shader=%s samplers=%zu variants=%zu",base->key.c_str(),requests.size(),border_variants.size());
    return result;
}
void resolve_border_variants(bool compute){
    for(unsigned stage=compute?5:0;stage<(compute?6:5);++stage)
        if(state.shader[stage])state.shader[stage]=border_variant(state.shader[stage]);
}

Buffer border_colors(const Shader& shader,unsigned ns){
    std::vector<float> colors;
    auto pool=samplers.find(pool_key(&state.sampler_pool));auto& table=pool==samplers.end()?legacy_samplers:pool->second;
    for(const auto& b:shader.border_sources){
        const uint64_t handle=b.sampler_slot==UINT32_MAX?state.tex[ns][b.slot]:state.samp[ns][b.sampler_slot];
        auto found=table.find(int(handle>>20));need(found!=table.end(),"Border color snapshot sampler missing");
        colors.insert(colors.end(),std::begin(found->second.border),std::end(found->second.border));
    }
    need(!colors.empty(),"Empty custom border color snapshot");
    return stream_buffer(colors.size()*sizeof(float),colors.data(),colors.size()*sizeof(float));
}
