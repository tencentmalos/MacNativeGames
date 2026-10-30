# SPDX-License-Identifier: MIT
"""Integrate host-only preparation plans into the build-local pinned renderer."""


def adapt(text, adapter):
    def replace(old, new, count=1):
        nonlocal text
        if text.count(old) != count:
            raise SystemExit(f"Pinned preparation patch mismatch: {old[:120]}")
        text = text.replace(old, new)

    text = '#include "' + str(adapter / 'preparation_plan.h') + '"\n' + text
    replace('struct Shader {bool uniform_counts_known=',
            'struct Shader {mng::vulkan::preparation::ShaderPlan<xbg_shader_binding> preparation;bool uniform_counts_known=')
    replace('} state;\n', '} state;\n#include "' + str(adapter / 'preparation_plan_engine.inc') + '"\n')
    replace('program_cache.clear();shader_families.clear();border_variants.clear();',
            'mng_prepare::reset();program_cache.clear();shader_families.clear();border_variants.clear();')
    replace('wtrack::enabled=false;state=State{};', 'wtrack::enabled=false;mng_prepare::reset();state=State{};')
    replace('warm::job_counters();spatial::LiteTrace::counter("NVN.Translate.ElapsedNs",elapsed);',
            'warm::job_counters();mng_prepare::counters();spatial::LiteTrace::counter("NVN.Translate.ElapsedNs",elapsed);')

    fixed = '&mng_prepare::versions.fixed'
    vertex = '&mng_prepare::versions.vertex'
    blend = '&mng_prepare::versions.blend'
    replace('state.blend[b.target]=b;', f'mng_prepare::assign(state.blend[b.target],b,{{{fixed},{blend}}});')
    replace('case 1:memcpy(&state.color,a.bytes,sizeof(state.color));break;',
            'case 1:{color_st next;memcpy(&next,a.bytes,sizeof next);if(next.alpha_func!=state.color.alpha_func)mng_prepare::bump(mng_prepare::versions.support);'
            f'mng_prepare::assign(state.color,next,{{{fixed},{blend}}});break;}}')
    for name, revisions in [('mask', f'{fixed},{blend}'), ('ms', fixed), ('poly', fixed), ('ds', fixed)]:
        replace(f'memcpy(&state.{name},a.bytes,sizeof(state.{name}));',
                f'mng_prepare::assign_bytes(&state.{name},a.bytes,sizeof(state.{name}),{{{revisions}}});')
    replace('need(xbg_vertex_attributes_replace(state.attrib,reinterpret_cast<const uint32_t*>(a.st),a.n)==0,"Invalid vertex attribute replacement");',
            'uint32_t next[16]{};need(xbg_vertex_attributes_replace(next,reinterpret_cast<const uint32_t*>(a.st),a.n)==0,"Invalid vertex attribute replacement");'
            f'mng_prepare::assign_bytes(state.attrib,next,sizeof next,{{{fixed},{vertex}}});')
    replace('memcpy(state.stream+a.first,a.st,a.n*sizeof(vs_st));',
            f'mng_prepare::assign_bytes(state.stream+a.first,a.st,a.n*sizeof(vs_st),{{{fixed},{vertex}}});')
    replace('(h.op==OP_BIND_UBO?state.ub:state.sb)[a.stage][a.index]={a.addr,a.size};++buf_gen[a.stage];',
            'auto &revision=(h.op==OP_BIND_UBO?mng_prepare::versions.uniform:mng_prepare::versions.storage)[a.stage][a.index];'
            'mng_prepare::assign_range((h.op==OP_BIND_UBO?state.ub:state.sb)[a.stage][a.index],NVNbufferRange{a.addr,a.size},revision);++buf_gen[a.stage];')
    replace('memcpy(dst+a.first,a.h,a.n*8);++tex_gen[a.stage];',
            'auto &revision=(h.op==OP_BIND_IMG?mng_prepare::versions.images:h.op==OP_BIND_SAMP?mng_prepare::versions.samplers:mng_prepare::versions.textures)[a.stage];'
            'mng_prepare::assign_bytes(dst+a.first,a.h,a.n*8,{&revision});++tex_gen[a.stage];')
    replace('if(memcmp(&state.rt,&next,sizeof next)){mng_feedback::targets_changed();last_pass.valid=false;}',
            'if(memcmp(&state.rt,&next,sizeof next)){mng_feedback::targets_changed();last_pass.valid=false;'
            'mng_prepare::bump(mng_prepare::versions.fixed);if(state.rt.n!=next.n)mng_prepare::bump(mng_prepare::versions.blend);}')
    replace('state.viewport.x=a.v[0][0];state.viewport.y=a.v[0][1];state.viewport.width=a.v[0][2];state.viewport.height=a.v[0][3];',
            'if(memcmp(&state.viewport.width,&a.v[0][2],sizeof(float)) || memcmp(&state.viewport.height,&a.v[0][3],sizeof(float)))mng_prepare::bump(mng_prepare::versions.support);'
            'state.viewport.x=a.v[0][0];state.viewport.y=a.v[0][1];state.viewport.width=a.v[0][2];state.viewport.height=a.v[0][3];')
    replace('state.texture_pool=*a.pool;', 'mng_prepare::assign(state.texture_pool,*a.pool,{&mng_prepare::versions.pools});')
    replace('state.sampler_pool=*a.pool;', 'mng_prepare::assign(state.sampler_pool,*a.pool,{&mng_prepare::versions.pools});')
    for target, source in [('stencil_compare[i]', 'a.value_mask'), ('stencil_write[i]', 'a.mask'), ('stencil_ref[i]', 'a.ref')]:
        replace(f'state.{target}={source};', f'mng_prepare::assign(state.{target},uint32_t({source}),{{{fixed}}});')
    replace('state.bias[0]=a.factor;state.bias[1]=a.units;state.bias[2]=a.clamp;',
            f'const float next[]{{a.factor,a.units,a.clamp}};mng_prepare::assign_bytes(state.bias,next,sizeof next,{{{fixed}}});')
    replace('memcpy(state.blend_color,a.rgba,16);', f'mng_prepare::assign_bytes(state.blend_color,a.rgba,16,{{{fixed}}});')
    replace('state.depth_clamp=payload<uint32_t>(p,n)!=0;', f'mng_prepare::assign(state.depth_clamp,payload<uint32_t>(p,n)!=0,{{{fixed}}});')
    replace('state.raster_discard=payload<uint32_t>(p,n)!=0;', f'mng_prepare::assign(state.raster_discard,payload<uint32_t>(p,n)!=0,{{{fixed}}});')
    replace('state.line_width=width;', f'mng_prepare::assign(state.line_width,width,{{{fixed}}});')
    replace('state.patch_size=size;', f'mng_prepare::assign(state.patch_size,size,{{{fixed}}});')
    replace('state.bounds=payload<xbg_depth_bounds>(p,n);', f'mng_prepare::assign(state.bounds,payload<xbg_depth_bounds>(p,n),{{{fixed}}});')
    # Typed clears temporarily change attachment views outside the opcode path.
    replace('const auto saved=state.rt;', 'mng_prepare::invalidate();const auto saved=state.rt;')
    replace('state.rt=saved;', 'state.rt=saved;mng_prepare::invalidate();')

    replace('unsigned dyn_mode_for(bool compute,bool *no_ubo=nullptr){unsigned all=0,vary=0;\n    for(',
            'unsigned dyn_mode_for(bool compute,bool *no_ubo=nullptr){unsigned all=0,vary=0;\n'
            '    if(auto *plan=mng_prepare::bindings(compute)){all=plan->uniforms;vary=plan->varying_uniforms;}else for(')

    # Keep the original key builder intact for the off path and exact validation.
    start = text.index('        for(unsigned st=0;st<5 && ok;st++)if(state.shader[st]){Shader &shader=*state.shader[st];')
    end = text.index('        if(ok){memo_check_epoch();', start)
    original = text[start:end]
    legacy = ('bool mng_descriptor_key_legacy(std::vector<uint64_t> &key,std::vector<uint8_t> &ktag,bool tags,unsigned dmode,std::vector<DynUbo> &dyn_ubos){bool ok=true;\n'
              + original + 'return ok;\n}\n#include "' + str(adapter / 'preparation_key_engine.inc') + '"\n')
    replace(original, '        ok=mng_descriptor_key(key,ktag,tags,dmode,dyn_ubos);\n')
    replace('void descriptors(Descriptors &out,Commands &commands,bool compute){', legacy + 'void descriptors(Descriptors &out,Commands &commands,bool compute){')
    # Reservation and dynamic-mode metadata come from the same immutable plan.
    compute_count = 'size_t elements=0;for(unsigned st=0;st<6;st++)if(state.shader[st] && (st==5)==compute)for(auto &b:state.shader[st]->bindings)elements+=b.count;'
    replace(compute_count, 'size_t elements=mng_prepare::element_count(compute);', 2)
    replace('size_t elements=0;for(unsigned st=0;st<5;st++)if(state.shader[st])for(auto &b:state.shader[st]->bindings)elements+=b.count;dm.reserve=',
            'size_t elements=mng_prepare::element_count(false);dm.reserve=')
    replace('struct Range{uint64_t begin,end;bool written;};static std::vector<Range> ranges,merged;static std::vector<uint32_t> key;ranges.clear();merged.clear();key.clear();',
            'using Range=mng::vulkan::preparation::StorageRange;static std::vector<Range> ranges,merged;static std::vector<uint32_t> key;ranges.clear();merged.clear();key.clear();'
            'const auto *prepared_ranges=mng_prepare::storage_ranges(compute);')
    replace('for(unsigned st=0;st<6;st++)if(state.shader[st] && (st==5)==compute)for(auto &b:state.shader[st]->bindings){\n        need(b.count',
            'mng_prepare::groups(compute,[&](Shader&,unsigned st,unsigned ns,const xbg_shader_binding &b,bool written){\n        need(b.count')
    replace('if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && (b.source==XBG_SOURCE_STORAGE || b.source==XBG_SOURCE_UNIFORM)){bool written=state.shader[st]->storage_writes.count(b.binding)!=0;for(unsigned i=0;i<b.count;i++){',
            'if(!prepared_ranges && b.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && (b.source==XBG_SOURCE_STORAGE || b.source==XBG_SOURCE_UNIFORM)){for(unsigned i=0;i<b.count;i++){')
    replace('ranges.push_back({r.address,r.address+r.size,written});\n        }}\n    }\n    std::sort(ranges.begin(),ranges.end(),',
            'ranges.push_back({r.address,r.address+r.size,written});\n        }}return true;\n    });\n    std::sort(ranges.begin(),ranges.end(),')
    replace('auto r=b.source==XBG_SOURCE_STORAGE?state.sb[nvn_stage[st]][b.slot+i]:state.ub[nvn_stage[st]][b.slot+i];',
            'auto r=b.source==XBG_SOURCE_STORAGE?state.sb[ns][b.slot+i]:state.ub[ns][b.slot+i];')
    # Prepared storage shapes are already merged. Avoid sorting them again.
    replace('std::sort(ranges.begin(),ranges.end(),[](const Range &a,const Range &b){return a.begin<b.begin;});\n    for(auto &r:ranges){if(!merged.empty() && r.begin<merged.back().end){merged.back().end=std::max(merged.back().end,r.end);merged.back().written=merged.back().written||r.written;}else merged.push_back(r);}',
            'if(prepared_ranges)merged=*prepared_ranges;else{mng::vulkan::preparation::merge_ranges(ranges);merged=ranges;}')
    replace('for(unsigned st=0;st<6;st++)if(state.shader[st] && (st==5)==compute){Shader &shader=*state.shader[st];unsigned ns=nvn_stage[st];\n        for(auto &b:shader.bindings)for(unsigned element=0;element<b.count;element++){DESC_SCOPE(ElementRest);',
            'mng_prepare::elements(compute,[&](Shader &shader,unsigned st,unsigned ns,const xbg_shader_binding &b,unsigned element,bool){DESC_SCOPE(ElementRest);')
    replace('            writes.push_back(write);\n        }\n    }\n    // Dynamic offsets are consumed in binding/array-element order.',
            '            writes.push_back(write);return true;\n    });\n    // Dynamic offsets are consumed in binding/array-element order.')

    # The compact guard does not replace the complete key or its invalidations.
    replace('bool memo_checked=false;', 'bool memo_checked=false;')  # anchor the existing path
    replace('pipe_memo_fill(pipe_memo_next,&d.entry->definition,p.definition,p.pass,d.pipeline_layout,prim,p.samples,active_colors);memo_checked=true;\n        if(pipe_memo_valid && !memcmp(&pipe_memo_next,&pipe_memo_key,sizeof pipe_memo_next)){',
            'const bool prepared=mng_draw_plan::pipeline.matches(&d.entry->definition,p.definition,p.pass,d.pipeline_layout,prim,p.samples,active_colors);'
            'if(!prepared || mng_prepare::validate())pipe_memo_fill(pipe_memo_next,&d.entry->definition,p.definition,p.pass,d.pipeline_layout,prim,p.samples,active_colors);memo_checked=!prepared || mng_prepare::validate();'
            'if(prepared && mng_prepare::validate())need(pipe_memo_valid && !memcmp(&pipe_memo_next,&pipe_memo_key,sizeof pipe_memo_next),"Preparation pipeline guard differs from full key");\n'
            '        if(pipe_memo_valid && (prepared || !memcmp(&pipe_memo_next,&pipe_memo_key,sizeof pipe_memo_next))){if(prepared)++mng_prepare::counts.pipeline_reuses;')
    replace('}pipeline_prepare.reset();',
            '}pipeline_prepare.reset();if(memo && pipe_memo_valid && pipeline.pipeline)mng_draw_plan::pipeline.remember(&d.entry->definition,p.definition,p.pass,d.pipeline_layout,prim,p.samples,active_colors);')
    text += r'''
extern "C" int mng_preparation_probe(char *out,unsigned bytes){
    std::lock_guard<std::recursive_mutex> lock(mutex);const auto &p=mng_prepare::counts;
    snprintf(out,bytes,"preparation plans=%u shader=%llu binding=%llu key=%llu/%llu ranges=%llu/%llu pipeline=%llu fallbacks=%llu",
        unsigned(mng_translate::preparation_plan()),(unsigned long long)p.shader_builds,(unsigned long long)p.binding_builds,
        (unsigned long long)p.key_builds,(unsigned long long)p.key_reuses,(unsigned long long)p.range_builds,
        (unsigned long long)p.range_reuses,(unsigned long long)p.pipeline_reuses,(unsigned long long)p.fallbacks);
    const bool exercised=p.binding_builds && p.range_builds &&
        (!mng_translate::descriptor_plan() || p.key_reuses) &&
        (!mng_translate::pipeline_early() || !mng_draw_plan::enabled() || p.pipeline_reuses);
    return mng_translate::preparation_plan() && !exercised?-1:0;
}
'''
    return text
