// SPDX-License-Identifier: MIT
// Bounded diagnostics only. These observations never suppress a dependency.
namespace mng_read_hazards {
struct Attachment {
    Image *image{};
    uint32_t mip{}, layer{};
    unsigned slot{};
    unsigned slots{};
    bool written{}, unknown{};
};
std::array<Attachment, 9> attachments{};
unsigned count=0, notes=0;
std::set<std::array<uint64_t, 9>> seen;
bool enabled() {
    static const bool value=[] {const char *v=getenv("MNG_IMAGE_READ_DIAGNOSTICS");return v && !strcmp(v,"1");}();
    return value;
}
void begin(const ImageList &actual, bool draw) {
    if(!enabled())return;
    count=0;
    for(unsigned slot=0;slot<9;++slot) {
        const tex_t *t=slot==8?state.rt.depth:slot<unsigned(state.rt.n)?state.rt.color[slot]:nullptr;
        if(!t)continue;
        auto it=images.find(t->serial);
        if(it==images.end() || std::find(actual.begin(),actual.end(),it->second.get())==actual.end())continue;
        const view_t *v=slot==8?(state.rt.has_dview?&state.rt.dview:nullptr):
            (state.rt.has_cview[slot]?&state.rt.cview[slot]:nullptr);
        const uint32_t mip=v&&v->set_levels?v->base_level:0, layer=v&&v->set_layers?v->min_layer:0;
        // Duplicate NVN slots can name the same physical attachment. Report
        // that attachment once and union its possible writes across aliases.
        unsigned existing=0;
        for(;existing<count;++existing)if(attachments[existing].image==it->second.get() &&
            attachments[existing].mip==mip && attachments[existing].layer==layer)break;
        if(existing<count)attachments[existing].slots|=1u<<slot;
        else attachments[count++]={it->second.get(),mip,layer,slot,1u<<slot,false,!draw};
    }
}
void draw() {
    if(!enabled())return;
    for(unsigned i=0;i<count;++i) {
        auto &a=attachments[i];
        for(unsigned slot=0;slot<9;++slot)if(a.slots&(1u<<slot))
            a.written|=slot<8?bool((state.mask.mask>>(4*slot))&15):
                bool((state.ds.dt && state.ds.dw) || state.ds.st);
    }
}
void clear() {
    if(!enabled())return;
    // Folded clears share draw passes. Until the clear's exact target/aspect
    // is recorded here, never classify those attachments as proven unwritten.
    for(unsigned i=0;i<count;++i)attachments[i].unknown=true;
}
void read(Image &im,const view_t *v,uint32_t flags) {
    if(!enabled() || !open_pass.cmd)return;
    uint32_t mip=v&&v->set_levels?v->base_level:0, mips=v&&v->set_levels?v->nlevels:im.levels;
    uint32_t layer=v&&v->set_layers?v->min_layer:0, layers=v&&v->set_layers?v->nlayers:im.layers;
    // Match Image::view's sampled 2D/array type correction. Volume slice
    // overlap remains unknown because slices are not Vulkan array layers.
    if((flags&(1u<<31)) && ((flags>>8)&7)==1 && !(flags&(1u<<13)) && im.samples==VK_SAMPLE_COUNT_1_BIT) {
        int target=v&&v->set_target?v->target:im.source.d.target;
        bool array=target==4 || target==6;
        if(target!=2 && target!=0 && target!=3 && target!=8 && target!=9 && array!=bool(flags&(1u<<12)))layers=1;
    }
    for(unsigned i=0;i<count;++i) {
        const auto &a=attachments[i];if(a.image!=&im)continue;
        const bool overlap=a.mip>=mip && uint64_t(a.mip)<uint64_t(mip)+mips &&
            a.layer>=layer && uint64_t(a.layer)<uint64_t(layer)+layers;
        const unsigned classification=(overlap?1:0)|(a.written?2:0)|(a.slot==8?8:4)|
            (a.unknown || im.source.d.target==2?16:0);
        spatial::LiteTrace::counter("NVN.ImageRead.Attachment",classification);
        if(notes>=128)return;
        std::array<uint64_t,9> key{im.source.serial,a.slot,mip,mips,layer,layers,a.mip,a.layer,classification};
        if(!seen.insert(key).second)continue;
        ++notes;
        xbg_logf("NVN read hazard serial=%llu slot=%u class=%u read_mip=%u+%u read_layer=%u+%u attachment_mip=%u attachment_layer=%u format=%#x target=%d extent=%ux%u current_mask=%#x current_depth=%u/%u/%u vs=%s fs=%s",
            (unsigned long long)im.source.serial,a.slot,classification,mip,mips,layer,layers,a.mip,a.layer,
            im.source.d.format,im.source.d.target,im.width,im.height,state.mask.mask,
            unsigned(state.ds.dt),unsigned(state.ds.dw),unsigned(state.ds.st),
            state.shader[0]?state.shader[0]->key.c_str():"none",state.shader[4]?state.shader[4]->key.c_str():"none");
    }
}
}
