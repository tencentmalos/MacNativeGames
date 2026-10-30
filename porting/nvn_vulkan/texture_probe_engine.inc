// SPDX-License-Identifier: MIT
// Included after Image. Debug-only, bounded snapshots of matching CPU uploads.
uint64_t texture_probe_source_serial=0;
void texture_upload_probe(const tex_t &t,const Image &im,const std::vector<uint8_t> &linear){
#ifndef NDEBUG
    struct Probe {
        const char *directory{};
        unsigned format{},width{},height{},layers{},count{};
        size_t bytes{};
        Probe(){
            const char *filter=getenv("MNG_TEXTURE_PROBE");
            if(filter && sscanf(filter,"%u,%u,%u,%u",&format,&width,&height,&layers)==4)
                directory=getenv("MNG_TEXTURE_PROBE_DIR");
        }
    };
    static Probe probe;
    constexpr size_t limit=96u<<20;
    if(!probe.directory || probe.count>=2 || (t.serial!=texture_probe_source_serial && (unsigned(t.d.format)!=probe.format || im.width!=probe.width ||
       im.height!=probe.height || im.layers!=probe.layers)) || t.L.size>limit || linear.size()>limit-t.L.size ||
       probe.bytes>limit-t.L.size-linear.size())return;
    const unsigned index=probe.count++;
    probe.bytes+=t.L.size+linear.size();
    char stem[768];snprintf(stem,sizeof stem,"%s/upload-%u-%llu",probe.directory,index,(unsigned long long)t.serial);
    auto save=[&](const char *suffix,const void *data,size_t bytes){
        std::string path=std::string(stem)+suffix;FILE *file=fopen(path.c_str(),"wb");
        if(!file)return false;
        bool ok=fwrite(data,1,bytes,file)==bytes;ok=fclose(file)==0 && ok;return ok;
    };
    bool raw_ok=save(".guest.bin",t.mem,t.L.size),linear_ok=save(".linear.bin",linear.data(),linear.size());
    char metadata[1536];int n=snprintf(metadata,sizeof metadata,
        "{\"schema\":1,\"serial\":%llu,\"image_serial\":%llu,\"format\":%d,\"target\":%d,"
        "\"width\":%u,\"height\":%u,\"depth\":%u,\"levels\":%u,\"layers\":%u,"
        "\"flags\":%u,\"stride\":%u,\"packaged_block\":%u,\"tile_h\":%u,\"tile_d\":%u,"
        "\"layer_bytes\":%llu,\"guest_bytes\":%llu,\"linear_bytes\":%zu,\"gpu_written\":%s,"
        "\"raw_complete\":%s,\"linear_complete\":%s}\n",
        (unsigned long long)t.serial,(unsigned long long)im.source.serial,t.d.format,t.d.target,
        im.width,im.height,im.depth,im.levels,im.layers,unsigned(t.d.flags),unsigned(t.d.stride),
        unsigned(t.d.pkg_block_h),unsigned(t.L.tileH),unsigned(t.L.tileD),
        (unsigned long long)t.L.layer_size,(unsigned long long)t.L.size,linear.size(),
        im.gpu_written?"true":"false",raw_ok?"true":"false",linear_ok?"true":"false");
    if(n>0 && size_t(n)<sizeof metadata)save(".json",metadata,n);
    xbg_logf("Texture upload probe %s raw=%d linear=%d total=%zu",stem,raw_ok,linear_ok,probe.bytes);
#endif
}

// Trace the producer of a selected attachment without reading GPU memory or
// changing command order. A fresh process is needed for another bounded run.
void texture_writer_probe(const tex_t &t,const view_t *v,const char *vertex,const char *fragment,uint64_t frame,int clear) {
#ifndef NDEBUG
    static unsigned count=0;
    static const char *filter=getenv("MNG_TEXTURE_PROBE");
    unsigned f,w,h,l;
    if(!filter || count>=48 || sscanf(filter,"%u,%u,%u,%u",&f,&w,&h,&l)!=4 ||
       unsigned(t.d.format)!=f || t.d.width!=w || t.d.height!=h || t.L.layers!=l)return;
    ++count;
    xbg_logf("Texture producer frame=%llu serial=%llu mem=%p format=%u extent=%ux%u layers=%u levels=%u mip=%d layer=%d clear=%d vs=%s fs=%s",
        (unsigned long long)frame,(unsigned long long)t.serial,t.mem,f,w,h,l,t.d.levels,
        v&&v->set_levels?v->base_level:0,v&&v->set_layers?v->min_layer:0,clear,vertex,fragment);
#endif
}
