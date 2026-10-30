// SPDX-License-Identifier: MIT
// Adapted from MacNativeGames nvn_obj.h rt_dup_of, which credits the MIT
// seedds/nxnative implementation. BOTW binds duplicate G-buffer slots with
// disabled writes. Keep only one physical attachment, and preserve clears.
int duplicate_color_slot(const p_set_rt& rt,int slot){
    if(slot<0 || slot>=rt.n || slot>=8 || !rt.color[slot])return -1;
    const auto key=[&](int i){
        const auto* t=rt.color[i];const auto* v=rt.has_cview[i]?&rt.cview[i]:nullptr;
        return std::array<uint64_t,4>{t->serial,
            uint64_t(v&&v->set_levels?v->base_level:0),
            uint64_t(v&&v->set_layers?v->min_layer:0),
            uint64_t(v&&v->set_format?v->format:t->d.format)};
    };
    const auto wanted=key(slot);
    for(int i=0;i<slot;++i)if(rt.color[i] && key(i)==wanted)return i;
    return -1;
}
void validate_duplicate_color_writes(){
    for(int i=1;i<state.rt.n;++i){
        const int first=duplicate_color_slot(state.rt,i);if(first<0)continue;
        if((state.mask.mask>>(4*i))&15)
            xbg_fatalf("Duplicate writable color attachments first=%d duplicate=%d serial=%llu mask=%#x",
                first,i,(unsigned long long)state.rt.color[i]->serial,state.mask.mask);
        static unsigned notes=0;if(notes++<8)
            xbg_logf("NVN duplicate color attachment first=%d duplicate=%d serial=%llu mask=%#x: disabled slot unattached",
                first,i,(unsigned long long)state.rt.color[i]->serial,state.mask.mask);
    }
}
// Detach a sample-only target, while retaining an inactive target that is not
// sampled. This preserves same-target continuation across mask/test changes.
// Unknown shader metadata keeps the conservative masked-color behavior.
// Bound extents still participate in render-area planning; clears retain all
// required targets independently of draw masks.
uint32_t draw_attachment_mask(){
    static const bool enabled=[] {const char *v=getenv("MNG_PRUNE_MASKED_ATTACHMENTS");return !v || strcmp(v,"0");}();
    uint32_t active=0;
    for(int i=0;i<state.rt.n;++i){
        const bool sampled=!mng_feedback::sampled_attachments_known || (mng_feedback::sampled_attachments&(1u<<i));
        if(!enabled || ((state.mask.mask>>(4*i))&15) || !sampled)active|=1u<<i;
    }
    // A bound depth image is not used by rasterization with every depth /
    // stencil test disabled. Keep its extent in the framebuffer plan, but
    // let shaders sample it without falsely treating it as a writable target.
    // Explicit clears use the full attachment mask independently.
    const bool depth_sampled=mng_feedback::sampled_attachments_known && (mng_feedback::sampled_attachments&(1u<<8));
    if(!enabled || state.ds.dt || state.ds.st || state.bounds.enable || !depth_sampled)active|=1u<<8;
    return active;
}
