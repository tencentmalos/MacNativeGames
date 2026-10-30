// SPDX-License-Identifier: MIT
// Included after the legacy key builder. Only the key is cached, never resources.
bool mng_descriptor_key(std::vector<uint64_t> &key,std::vector<uint8_t> &tags,bool tagged,
                        unsigned mode,std::vector<DynUbo> &dynamic) {
    auto *plan=mng_translate::descriptor_plan()?mng_prepare::bindings(false):nullptr;
    if(!plan){if(mng_translate::preparation_plan()){++mng_prepare::counts.fallbacks;++mng_prepare::counts.legacy_key_fallbacks;}
        return mng_descriptor_key_legacy(key,tags,tagged,mode,dynamic);}
    auto &cache=plan->keys[mode];
    const uint64_t epoch=memo_epoch.load(std::memory_order_relaxed);
    const bool reuse=cache.ready && cache.ok && cache.texel_enabled==texel_memo_enabled && cache.epoch==epoch && cache.watch.unchanged();
    if(!reuse){
        cache.words.clear();cache.tags.clear();
        cache.ok=mng_prepare::elements(false,[&](Shader&,unsigned,unsigned ns,const auto &b,unsigned element,bool written){
            if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER || (b.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER && !texel_memo_enabled)){++desc_memo_why[0];return false;}
            if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE){++desc_memo_why[1];return false;}
            if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && written){++desc_memo_why[2];return false;}
            auto &words=cache.words;auto &ktag=cache.tags;const unsigned slot=b.slot+element;
            switch(b.source){
            case XBG_SOURCE_UNIFORM:case XBG_SOURCE_STORAGE:{
                if(slot>=16)return false;
                const auto r=b.source==XBG_SOURCE_UNIFORM?state.ub[ns][slot]:state.sb[ns][slot];
                if(UBO_DYN(mode,b) && b.source==XBG_SOURCE_UNIFORM && r.address && r.size){
                    words.push_back(std::min<size_t>(r.size,b.bytes?b.bytes:65536));ktag.push_back(2);
                }else{words.push_back(r.address);words.push_back(r.size);ktag.push_back(b.source==XBG_SOURCE_UNIFORM?3:10);ktag.push_back(4);}
                break;}
            case XBG_SOURCE_SUPPORT:
                words.push_back((uint64_t(uint32_t(state.viewport.width))<<32)|uint32_t(state.viewport.height));
                words.push_back(state.color.alpha_func);ktag.push_back(5);ktag.push_back(6);break;
            case XBG_SOURCE_C1:break;
            case XBG_SOURCE_DRIVER0:case XBG_SOURCE_DRIVER2:
                words.push_back(UBO_DYN(mode,b)?uint64_t(0xD0D0+b.source):(b.source==XBG_SOURCE_DRIVER0?tex_gen[ns]:buf_gen[ns]));ktag.push_back(7);break;
            case XBG_SOURCE_TEXTURE:case XBG_SOURCE_IMAGE:
                if(slot>=(b.source==XBG_SOURCE_IMAGE?16u:128u))return false;
                words.push_back(b.source==XBG_SOURCE_IMAGE?state.img[ns][slot]:state.tex[ns][slot]);ktag.push_back(8);
                if(b.sampler_slot!=UINT32_MAX){if(b.sampler_slot>=128)return false;words.push_back(state.samp[ns][b.sampler_slot]);ktag.push_back(9);}
                break;
            default:return false;
            }
            return true;
        });
        cache.watch.remember();cache.epoch=epoch;cache.texel_enabled=texel_memo_enabled;cache.ready=true;++mng_prepare::counts.key_builds;
        if(!cache.ok)++mng_prepare::counts.key_ineligible;
    }else ++mng_prepare::counts.key_reuses;
    if(mng_prepare::validate()){
        std::vector<uint64_t> expected;std::vector<uint8_t> expected_tags;std::vector<DynUbo> unused;
        uint64_t previous_why[6];memcpy(previous_why,desc_memo_why,sizeof previous_why);
        const bool expected_ok=mng_descriptor_key_legacy(expected,expected_tags,true,mode,unused);
        memcpy(desc_memo_why,previous_why,sizeof previous_why);
        need(expected_ok==cache.ok && expected==cache.words && expected_tags==cache.tags,
             "Preparation descriptor key differs from legacy key");
    }
    key.insert(key.end(),cache.words.begin(),cache.words.end());
    if(tagged)tags.insert(tags.end(),cache.tags.begin(),cache.tags.end());
    return cache.ok;
}
