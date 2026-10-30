// SPDX-License-Identifier: MIT
// Included at the validated command-consumption boundary. No resource reads,
// shader loads or state changes are introduced by these diagnostic summaries.
void mng_record_nvn_command(uint64_t submission,const uint8_t* pc,uint32_t op,uint32_t size,const uint8_t* p,size_t n) {
    static const char* names[]={"End","Jump","Begin","Call","SignalEvent","ReportCounter","CopyBufferToBuffer","UpdateUniformBuffer",
        "SetRenderTargets","ClearColor","ClearDepthStencil","SetViewports","SetScissor","SetDepthRange","BindProgram","BindState",
        "BindVertexAttributes","BindVertexStreams","BindVertexBuffers","BindUniformBuffer","BindStorageBuffer","BindTextures","BindSamplers",
        "SetStencil","SetPolygonOffset","DrawArrays","DrawElements","DispatchCompute","Barrier","SetTexturePool","SetSamplerPool",
        "CopyBufferToTexture","SetCopyRowStride","SetBlendColor","CopyTextureToBuffer","BindImages","CopyTextureToTexture","ResetCounter",
        "ClearTexture","SetCopyImageStride","SetPatchSize","WaitSync","DispatchIndirect","MultiDrawIndirect","DrawTexture","SetDepthBounds",
        "Downsample","DrawArraysInstanced","DrawIndirect","ClearColorTyped","SetDepthClamp","SetLineWidth","SetRasterizerDiscard"};
    const char* name=op<sizeof(names)/sizeof(*names)?names[op]:"Unknown";
    std::string detail;
    auto texture=[](const tex_t* t){
        if(!t)return std::string("null");
        char text[320];snprintf(text,sizeof text,"serial=%llu,format=%d,target=%d,extent=%ux%ux%u,levels=%u,layers=%u,flags=%u,pkg=%d,tile=%u/%u,bytes=%llu",
            (unsigned long long)t->serial,t->d.format,t->d.target,t->d.width,t->d.height,t->d.depth,t->d.levels,t->L.layers,
            unsigned(t->d.flags),t->d.pkg_block_h,t->L.tileH,t->L.tileD,(unsigned long long)t->L.size);return std::string(text);
    };
    auto region=[](const NVNcopyRegion& r){char text[120];snprintf(text,sizeof text,"%d,%d,%d:%dx%dx%d",r.xoffset,r.yoffset,r.zoffset,r.width,r.height,r.depth);return std::string(text);};
    if(op==OP_COPY_T2T && n>=sizeof(p_copy_t2t)) {
        auto a=payload<p_copy_t2t>(p,n);detail="src{"+texture(a.src)+"} dst{"+texture(a.dst)+"} regions="+region(a.sr)+"->"+region(a.dr)+
            " mips="+std::to_string(a.has_sv&&a.sv.set_levels?a.sv.base_level:0)+"/"+std::to_string(a.has_dv&&a.dv.set_levels?a.dv.base_level:0);
    }else if((op==OP_COPY_B2T||op==OP_COPY_T2B)&&n>=sizeof(p_copy_b2t)){
        auto a=payload<p_copy_b2t>(p,n);detail="texture{"+texture(a.tex)+"} region="+region(a.r)+" row_stride="+std::to_string(state.copy_stride)+" image_stride="+std::to_string(state.copy_image_stride);
    }else if(op==OP_SET_RT&&n>=sizeof(p_set_rt)){
        auto a=payload<p_set_rt>(p,n);detail="depth{"+texture(a.depth)+"}";
        for(int i=0;i<std::min(a.n,8);++i)detail+=" color"+std::to_string(i)+"{"+texture(a.color[i])+"}";
    }else if((op==OP_BIND_UBO||op==OP_BIND_SSBO)&&n>=sizeof(p_bind_buf)){
        auto a=payload<p_bind_buf>(p,n);char text[160];snprintf(text,sizeof text,"stage=%d slot=%d address=0x%llx bytes=%llu",a.stage,a.index,(unsigned long long)a.addr,(unsigned long long)a.size);detail=text;
    }else {
        // Bounded raw scalar prefix for states/draw arguments. Pointers, when
        // present, are capture-process identities and are not replay addresses.
        char text[3];detail="payload_prefix=";for(size_t i=0;i<std::min(n,size_t(32));++i){snprintf(text,sizeof text,"%02x",p[i]);detail+=text;}
    }
    if(op==OP_DRAW||op==OP_DRAW_IDX||op==OP_DISPATCH||op==OP_COUNT_+2||op==OP_COUNT_+3||op==OP_COUNT_+7||op==OP_COUNT_+8) {
        for(unsigned stage=0;stage<6;++stage)if(state.shader_family[stage])
            detail+=" shader_family"+std::to_string(stage)+"="+state.shader_family[stage]->identity;
    }
    mng_render_record_action(submission,uint64_t(pc),op,size,name,detail.c_str());
}
