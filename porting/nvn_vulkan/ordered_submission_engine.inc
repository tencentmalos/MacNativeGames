// SPDX-License-Identifier: MIT
// Included after execute(). Capture walks a private copy of inherited binding
// state, not Vulkan commands. No execution state or GPU address is rewritten.
extern "C" bool mng_nvn_capture_report_output(uint64_t,mng::port::SubmissionInputs*);
bool mng_capture_ranges(mng::port::SubmissionInputs &out){
    if(!pending_events.empty() || !recording_events.empty())return false;
    return out.capture([](uint64_t address,uint64_t bytes){
        auto pool=pools.upper_bound(address);if(pool==pools.begin())return false;--pool;
        if(address-pool->first>pool->second || bytes>pool->second-(address-pool->first))return false;
        for(const auto &w:pending_writebacks)if(xbg_wb_overlaps(address,bytes,w.addr,w.bytes))return false;
        for(const auto &r:pending_reports)if(xbg_wb_overlaps(address,bytes,r.addr,sizeof(NVNcounterData)))return false;
        return true;
    });
}
extern "C" int mng_capture_present(const tex_t *texture,mng::port::SubmissionInputs *out){
    MngEngineLock lock(mutex);
    if(!texture || capture.active || provenance_file || mng_render_record_active() || getenv("MNG_TEXTURE_PROBE"))return 1;
    return out->add(uint64_t(texture->mem),texture->L.size) && mng_capture_ranges(*out)?0:6;
}
extern "C" int mng_capture_submission(int count,const uint64_t *handles,
                                      mng::port::SubmissionInputs *out,mng::port::SubmissionSite *site) {
    MngEngineLock lock(mutex);
    *site={};
    if(!initialized || failed || gfx_async || capture.active || provenance_file || g_ui_moff ||
       mng_render_record_active() || getenv("MNG_TEXTURE_PROBE"))return 1;
    auto local=state;
    mng::port::SubmissionInputs buffer_reads;
    std::map<uint64_t,uint64_t> image_writes;
    using Failure=mng::port::InputFailure;
    uint32_t input_stage=UINT32_MAX,input_binding=UINT32_MAX;
    auto reject=[&](Failure reason,uint64_t bytes=0){
        if(site->input_failure==Failure::None){site->input_failure=reason;site->stage=input_stage;
            site->binding=input_binding;site->requested_bytes=bytes;site->accumulated_bytes=out->pending_bytes();}
        return false;
    };
    auto buffer=[&](uint64_t address,uint64_t bytes){
        if(buffer_reads.add(address,bytes) && out->add(address,bytes))return true;
        return reject(Failure::RangeBudget,bytes);
    };
    auto target=[&](const tex_t *t){
        if(t && t->mem){auto &bytes=image_writes[uint64_t(t->mem)];bytes=std::max<uint64_t>(bytes,t->L.size);}
    };
    auto texture=[&](const tex_t *t){
        // Image writes remain ordered Vulkan operations. They are not copied
        // back into Guest storage here; buffer/texture alias reads fail closed.
        return !t || !t->mem || out->add(uint64_t(t->mem),t->L.size) || reject(Failure::RangeBudget,t->L.size);
    };
    auto reads=[&](bool compute)->bool {
        for(unsigned st=compute?5:0;st<(compute?6:5);++st){
            input_stage=st;input_binding=UINT32_MAX;
            auto *family=local.shader_family[st];Shader *shader=local.shader[st];
            if(family){
                shader=family->single;
                if(!shader){
                    auto answer=[&](const mng::shader::Condition &c){
                        if(c.kind==0){va_st a;memcpy(&a,&local.attrib[c.slot],sizeof a);return a.fmt?shader_scalar(a.fmt):-1;}
                        const auto *t=local.rt.color[c.slot];if(!t || c.slot>=uint32_t(local.rt.n))return 0;
                        const auto &v=local.rt.cview[c.slot];return shader_scalar(local.rt.has_cview[c.slot]&&v.set_format?v.format:t->d.format);
                    };
                    const auto *variant=mng::shader::select(family->variants,answer);
                    if(!variant)return reject(Failure::ShaderVariant);shader=load_shader_key(variant->key);
                }
            }
            if(!shader)continue;
            const unsigned ns=nvn_stage[st];
            for(const auto &b:shader->bindings){
                input_binding=b.binding;
                if(!b.count || b.count>128)return reject(Failure::Binding);
                // Writable storage may feed a later read in the same packet.
                // Do not replace that dependency with a stale CPU snapshot.
                if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || b.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER ||
                   (b.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && shader->storage_writes.count(b.binding)))return reject(Failure::WritableStorage,b.bytes);
                for(unsigned i=0;i<b.count;++i){
                    if(b.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER){
                        if(b.source!=XBG_SOURCE_UNIFORM && b.source!=XBG_SOURCE_STORAGE)continue;
                        if(b.slot+i>=16)return reject(Failure::Binding);
                        auto r=b.source==XBG_SOURCE_UNIFORM?local.ub[ns][b.slot+i]:local.sb[ns][b.slot+i];
                        uint64_t bytes=b.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER?std::min<uint64_t>(r.size,b.bytes?b.bytes:65536):r.size;
                        if(!buffer(r.address,bytes))return false;
                    }else{
                        if(b.source!=XBG_SOURCE_TEXTURE && b.source!=XBG_SOURCE_IMAGE)return reject(Failure::Binding);
                        if(b.slot+i>=(b.source==XBG_SOURCE_IMAGE?16u:128u))return reject(Failure::Binding);
                        const auto h=b.source==XBG_SOURCE_IMAGE?local.img[ns][b.slot+i]:local.tex[ns][b.slot+i];
                        const int id=int(h&0xfffff);
                        if(!id || (local.texture_pool.n>0 && id>=local.texture_pool.n)){
                            // Null image contents are renderer-owned. A null
                            // texel buffer still needs a distinct owned range.
                            if(b.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)return reject(Failure::Binding);
                            continue;
                        }
                        auto pool=textures.find(pool_key(&local.texture_pool));
                        const auto &table=pool==textures.end()?legacy_textures:pool->second;
                        auto found=table.find(id);if(found==table.end())return reject(Failure::Binding);
                        const auto &t=found->second.texture;
                        if(b.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER){if(!buffer(uint64_t(t.mem),t.L.size))return false;}
                        else if(!texture(&t))return false;
                    }
                }
            }
            if(st==0 && !compute)for(unsigned input:shader->inputs){
                input_binding=input;if(input>=16)return reject(Failure::Binding);va_st attr;memcpy(&attr,&local.attrib[input],sizeof attr);
                if(!attr.fmt)continue;
                if(attr.stream>=16)return reject(Failure::Binding);
                const auto r=local.vb[attr.stream];if(!buffer(r.address,r.size))return false;
            }
        }
        input_stage=input_binding=UINT32_MAX;
        // Attachment creation/alias resolution can inspect source storage too.
        if(!compute){
            const auto v=local.vb[0],u=local.ub[0][0];
            if(v.address && v.size==80 && u.address && u.size>=64 &&
               (!buffer(v.address,80) || !buffer(u.address,64)))return false;
            for(int i=0;i<local.rt.n;++i){target(local.rt.color[i]);if(!texture(local.rt.color[i]))return false;}
            target(local.rt.depth);if(!texture(local.rt.depth))return false;}
        return true;
    };
    auto index=[&](uint64_t address,int type,int count){
        input_stage=input_binding=UINT32_MAX;
        if(type<0 || type>2 || count<0)return reject(Failure::Other);
        return buffer(address,uint64_t(count)*(1u<<type));
    };
    for(int i=0;i<count;++i){
        auto *pc=reinterpret_cast<const uint8_t*>(handles[i]);
        size_t remaining=mng_nvn_command_available(pc);bool ended=false;
        while(remaining>=sizeof(op_hdr)){
            op_hdr h;memcpy(&h,pc,sizeof h);
            *site={h.op,uint32_t(i),uint64_t(pc-reinterpret_cast<const uint8_t*>(handles[i]))};
            if(h.size<sizeof h || (h.size&7) || h.size>remaining)return 3;
            const auto *p=pc+sizeof h;const size_t n=h.size-sizeof h;
            if(h.op==OP_END){ended=true;break;}
            switch(h.op){
            case OP_REPORT_COUNTER:{
                if(n<16)return 3;
                uint64_t type,address;memcpy(&type,p,8);memcpy(&address,p+8,8);
                if(type!=NVN_COUNTER_TYPE_TIMESTAMP && type!=NVN_COUNTER_TYPE_TIMESTAMP_TOP && type!=NVN_COUNTER_TYPE_SAMPLES_PASSED)return 8;
                if(!mng_nvn_capture_report_output(address,out))return 8;
                for(const auto &w:pending_writebacks)if(xbg_wb_overlaps(address,sizeof(NVNcounterData),w.addr,w.bytes))return 9;
                break;}
            case OP_BIND_PROGRAM:{
                const auto a=payload<p_bind_program>(p,n);
                for(unsigned st=0;st<6;++st)if(a.stages&(1u<<nvn_stage[st]))local.shader_family[st]=nullptr,local.shader[st]=nullptr;
                if(a.prog){const auto &loaded=program_shaders(a.prog);
                    for(unsigned j=0;j<a.prog->nstage;++j){auto *f=loaded[j];
                        if(a.stages&(1u<<nvn_stage[f->stage])){local.shader_family[f->stage]=f;local.shader[f->stage]=f->single;}}}
                break;}
            case OP_BIND_UBO:case OP_BIND_SSBO:{
                const auto a=payload<p_bind_buf>(p,n);if(a.stage<0 || a.stage>=6 || a.index<0 || a.index>=16)return 3;
                (h.op==OP_BIND_UBO?local.ub:local.sb)[a.stage][a.index]={a.addr,a.size};break;}
            case OP_BIND_VBUF:{
                auto a=variable_payload<p_bind_vbuf>(p,n);
                if(a.first<0 || a.n<0 || a.first>16 || a.n>16-a.first || n<offsetof(p_bind_vbuf,r)+size_t(a.n)*sizeof(NVNbufferRange))return 3;
                memcpy(local.vb+a.first,a.r,a.n*sizeof(NVNbufferRange));break;}
            case OP_BIND_VATTR:{
                auto a=variable_payload<p_bind_vattr>(p,n);
                if(a.first || a.n<0 || a.n>16 || n<offsetof(p_bind_vattr,st)+size_t(a.n)*4)return 3;
                if(xbg_vertex_attributes_replace(local.attrib,a.st,a.n))return 3;break;}
            case OP_BIND_TEX:case OP_BIND_IMG:{
                auto a=variable_payload<p_bind_handles>(p,n);const int cap=h.op==OP_BIND_IMG?16:128;
                if(a.stage<0 || a.stage>=6 || a.first<0 || a.first>cap || a.n<0 || a.n>32 || a.n>cap-a.first || n<offsetof(p_bind_handles,h)+size_t(a.n)*8)return 3;
                memcpy((h.op==OP_BIND_IMG?local.img[a.stage]:local.tex[a.stage])+a.first,a.h,a.n*8);break;}
            case OP_SET_TEX_POOL:{auto a=payload<p_set_pool>(p,n);if(!a.pool)return 3;local.texture_pool=*a.pool;break;}
            case OP_SET_RT:local.rt=payload<p_set_rt>(p,n);if(local.rt.n<0 || local.rt.n>8)return 3;break;
            case OP_DRAW:if(!reads(false))return 5;break;
            case OP_DRAW_IDX:{auto a=payload<p_draw_idx>(p,n);if(!reads(false) || !index(a.ibuf,a.itype,a.count))return 5;break;}
            case OP_COUNT_+7:{auto a=payload<xbg_draw_call>(p,n);if(!reads(false) || (a.index_type>=0 && !index(a.index_address,a.index_type,a.count)))return 5;break;}
            case OP_DISPATCH:if(!reads(true))return 5;break;
            // These commands change only inline state or Vulkan state. All
            // other operations (including CPU writes, events, copies and waits)
            // use serial execution; new opcodes are unsupported by default.
            case OP_BEGIN:case OP_BIND_STATE:case OP_BIND_VSTREAM:case OP_BIND_SAMP:
            case OP_SET_SAMP_POOL:case OP_VIEWPORTS:case OP_SCISSOR:case OP_DEPTH_RANGE:
            case OP_STENCIL:case OP_POLY_OFFSET:case OP_BARRIER:case OP_BLEND_COLOR:
            case OP_COPY_STRIDE:case OP_COPY_IMAGE_STRIDE:case OP_COUNT_:case OP_COUNT_+5:
            case OP_COUNT_+10:case OP_COUNT_+11:case OP_COUNT_+12:break;
            case OP_CLEAR_COLOR:case OP_CLEAR_DS:case OP_COUNT_+9:
                for(int j=0;j<local.rt.n;++j){target(local.rt.color[j]);if(!texture(local.rt.color[j]))return 2;}
                target(local.rt.depth);if(!texture(local.rt.depth))return 2;break;
            default:return 4;
            }
            pc+=h.size;remaining-=h.size;
        }
        if(!ended)return 3;
    }
    if(!out->reports_disjoint())return 9;
    for(const auto &[address,bytes]:image_writes)if(buffer_reads.overlaps(address,bytes)){input_stage=input_binding=UINT32_MAX;reject(Failure::Alias,bytes);return 5;}
    return mng_capture_ranges(*out)?0:6;
}
