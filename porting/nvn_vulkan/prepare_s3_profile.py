# SPDX-License-Identifier: MIT
"""Sample one complete draw preparation chain; never alter renderer semantics."""


def adapt(text, adapter):
    def replace(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise SystemExit(f"Pinned S3 diagnostic patch mismatch: {old[:120]}")
        text = text.replace(old, new)

    def scope(name, variable):
        return (f'spatial::LiteTraceScope {variable};'
                f'if(mng::profile::detailedCpu()){variable}.begin("{name}");')

    def cpu_scope(name, kind):
        return scope(name, 'ring_scope') + f'XbgCpuTiming::Scope cpu_scope(timing_now(),XbgCpuTiming::{kind});'

    # Select before preflight, so its detail shares the draw's 1/N decision.
    replace('mng_reads::Guard read_guard;mng::profile::CpuDetailDrawScope detail_draw;CPU_SCOPE(Draw);',
            'mng_reads::Guard read_guard;CPU_SCOPE(Draw);')
    replace('void draw(int prim,int first,int count,const p_draw_idx *indexed,uint32_t instances=1,uint32_t first_instance=0){texture_source_probe();',
            'void draw(int prim,int first,int count,const p_draw_idx *indexed,uint32_t instances=1,uint32_t first_instance=0){mng::profile::CpuDetailDrawScope detail_draw;'
            + scope('NVN.Draw.CallSample', 'call_sample') + 'texture_source_probe();')
    replace('void descriptors(Descriptors &out,Commands &commands,bool compute){',
            'void descriptors(Descriptors &out,Commands &commands,bool compute){' + scope('NVN.Draw.Descriptors', 'descriptor_scope'))
    replace('if(ok){CPU_SCOPE(Descriptor);mng_feedback::key(key);',
            'if(ok){' + cpu_scope('NVN.Descriptor.Memo', 'Descriptor') + 'mng_feedback::key(key);')
    replace('void descriptors_build(Descriptors &out,Commands &commands,bool compute){CPU_SCOPE(Descriptor);',
            'void descriptors_build(Descriptors &out,Commands &commands,bool compute){' + cpu_scope('NVN.Descriptor.Build', 'Descriptor'))
    replace('ok=mng_descriptor_key(key,ktag,tags,dmode,dyn_ubos);',
            '{' + scope('NVN.Descriptor.Key', 'key_scope') + 'ok=mng_descriptor_key(key,ktag,tags,dmode,dyn_ubos);}')
    replace('for(auto &d:m.dyn){if(!d.uniform){',
            '{' + scope('NVN.Descriptor.Dynamic', 'dynamic_scope') + 'for(auto &d:m.dyn){if(!d.uniform){')
    replace('same=same && fi==fresh.size();', '}same=same && fi==fresh.size();')
    image_loop='for(auto &ti:m.images){Image &im=get_image(ti.first,true);prepare_read(mng_feedback::sample(im),commands.command,ti.record->has_view?&ti.record->view:nullptr,ti.flags);}'
    replace(image_loop, '{' + scope('NVN.Descriptor.Images', 'images_scope') + image_loop + '}')
    replace('Buffer uniform_snapshot_cached(uint64_t address,size_t copy){CPU_SCOPE(Snapshot);',
            'Buffer uniform_snapshot_cached(uint64_t address,size_t copy){' + cpu_scope('NVN.Snapshot.Uniform', 'Snapshot'))
    replace('Buffer snapshot(uint64_t address,size_t bytes,VkDeviceSize align=4){CPU_SCOPE(Snapshot);',
            'Buffer snapshot(uint64_t address,size_t bytes,VkDeviceSize align=4){spatial::LiteTraceScope ring_scope;'
            'if(mng::profile::detailedCpu())ring_scope.begin(snap_site==1?"NVN.Snapshot.Vertex":snap_site==2?"NVN.Snapshot.Index":snap_site==3?"NVN.Snapshot.Storage":"NVN.Snapshot.Other");'
            'XbgCpuTiming::Scope cpu_scope(timing_now(),XbgCpuTiming::Snapshot);')
    replace('{DRAW_SCOPE(VertexPrepare);for(auto &b:bindings){',
            '{DRAW_SCOPE(VertexPrepare);' + scope('NVN.Draw.VertexIndex', 'vertex_scope') + 'for(auto &b:bindings){')
    return text
