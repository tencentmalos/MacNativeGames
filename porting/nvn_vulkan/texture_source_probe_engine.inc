// SPDX-License-Identifier: MIT
// Bounded source identities and CPU-backed snapshots; no GPU synchronization.
void texture_source_probe(){
#ifndef NDEBUG
    static std::set<uint64_t> seen;static unsigned snapshots=0;
    static const char *filter=getenv("MNG_TEXTURE_PROBE");
    unsigned f,w,h,l;
    if(seen.size()>=96 || !filter || sscanf(filter,"%u,%u,%u,%u",&f,&w,&h,&l)!=4)return;
    for(int i=0;i<state.rt.n;++i){const auto* t=state.rt.color[i];
        if(!t || unsigned(t->d.format)!=f || t->d.width!=w || t->d.height!=h || t->L.layers!=l)continue;
        const auto handle=state.tex[nvn_stage[4]][0];const int id=int(handle&0xfffff);
        if(!id || !state.texture_pool.pool)continue;
        auto pool=textures.find(pool_key(&state.texture_pool));
        auto &table=pool==textures.end()?legacy_textures:pool->second;
        auto entry=table.find(id);if(entry==table.end())continue;
        const auto &r=entry->second;const auto &source=r.texture;if(!seen.insert(source.serial).second)continue;
        texture_probe_source_serial=source.serial;
        xbg_logf("Texture producer input frame=%llu handle=%#llx serial=%llu mem=%p format=%u extent=%ux%u layers=%u levels=%u stride=%u flags=%u tileH=%u tileD=%u bytes=%llu viewMip=%d viewLayer=%d viewFormat=%d",
            (unsigned long long)stats.presents.load(),(unsigned long long)handle,(unsigned long long)source.serial,source.mem,
            unsigned(source.d.format),source.d.width,source.d.height,source.L.layers,source.d.levels,source.d.stride,source.d.flags,
            source.L.tileH,source.L.tileD,(unsigned long long)source.L.size,
            r.has_view&&r.view.set_levels?r.view.base_level:0,r.has_view&&r.view.set_layers?r.view.min_layer:0,
            r.has_view&&r.view.set_format?r.view.format:source.d.format);
        // Preserve the CPU source even if it was uploaded before this draw. Do
        // not label backing bytes as GPU contents for a GPU-produced input.
        const auto *image=find_image(source.serial);
        const char *directory=getenv("MNG_TEXTURE_PROBE_DIR");
        int bw,bh;const int bpp=nvn_fmt_bpb(source.d.format,&bw,&bh);
        if(!directory || snapshots>=2 || source.mem==t->mem || (image&&image->gpu_written) ||
           source.d.width<=16 || source.d.height<=16 || source.L.layers!=1 || source.d.target!=1 ||
           source.d.samples>1 || bpp<=0 || bw!=1 || bh!=1 || source.L.size>(32u<<20))continue;
        const size_t pitch=size_t(source.d.width)*bpp,bytes=pitch*source.d.height;
        if(bytes>(32u<<20))continue;
        std::vector<uint8_t> linear(bytes);
        if(source.d.flags&0x50){const size_t row=mng_linear_stride(&source.d);
            if(row<pitch || row*source.d.height>source.L.size)continue;
            for(unsigned y=0;y<source.d.height;++y)memcpy(linear.data()+y*pitch,source.mem+y*row,pitch);
        }else if(nvn_deswizzle(&source.d,0,0,source.mem,linear.data(),pitch))continue;
        char stem[768];snprintf(stem,sizeof stem,"%s/input-%u-%llu",directory,snapshots++,(unsigned long long)source.serial);
        auto save=[&](const char *ext,const void *data,size_t n){FILE *file=fopen((std::string(stem)+ext).c_str(),"wb");if(!file)return false;bool ok=fwrite(data,1,n,file)==n;return fclose(file)==0&&ok;};
        bool raw=save(".guest.bin",source.mem,source.L.size),lin=save(".mip0.bin",linear.data(),bytes);
        char info[1024];int n=snprintf(info,sizeof info,"{\"schema\":1,\"source\":\"cpu_backing_at_first_height_draw\",\"serial\":%llu,\"format\":%d,\"width\":%u,\"height\":%u,\"bpp\":%d,\"pitch\":%zu,\"flags\":%u,\"tile_h\":%u,\"raw_complete\":%s,\"linear_complete\":%s}\n",(unsigned long long)source.serial,source.d.format,source.d.width,source.d.height,bpp,pitch,source.d.flags,source.L.tileH,raw?"true":"false",lin?"true":"false");
        if(n>0&&size_t(n)<sizeof info)save(".json",info,n);
        xbg_logf("Texture source probe %s raw=%d linear=%d",stem,raw,lin);
    }
#endif
}
