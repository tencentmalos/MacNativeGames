#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Apply bounded native-port changes to a build-local copy of the pinned renderer.
The original D2R checkout stays read-only. Its original license still applies.
"""
import pathlib
import re
import sys
source, destination = map(pathlib.Path, sys.argv[1:])
adapter = pathlib.Path(__file__).resolve().parent
text = source.read_text()
text = '#include "'+str(adapter/'runtime_policy.h')+'"\n#include "'+str(adapter/'flush_trace.h')+'"\n#include "'+str(adapter/'shader_archive.h')+'"\n'+text
text = '#include "'+str(adapter.parent/'runtime/performance_hint.h')+'"\n'+text
def replace(old, new, count=1):
    global text
    if text.count(old) != count:
        raise SystemExit(f"Pinned renderer patch mismatch: {old[:100]}")
    text = text.replace(old, new)
replace('extern "C" int xbg_game_initialize(const xbg_game_config *c,char *error,size_t bytes){',
        'extern "C" int xbg_game_initialize(const xbg_game_config *c,char *error,size_t bytes){\n    if(!mng::vulkan::policy::apply(error,bytes))return -1;')
# Immutable precompiled bundles avoid per-shader filesystem metadata work.
replace('const uint64_t load_t0=timing_clock();', 'spatial::LiteTraceScope shader_load("NVN.Shader.Load");spatial::LiteTrace::counter("NVN.Shader.Key64",strtoull(key.c_str(),nullptr,16));const uint64_t load_t0=timing_clock();')
replace('std::string path=std::string(cfg.shader_directory)+"/"+key+".xgs";FILE *f=fopen(path.c_str(),"rb");if(!f)xbg_fatalf("Missing genuine shader bundle %s",path.c_str());\n    fseek(f,0,SEEK_END);long length=ftell(f);rewind(f);need(length>=32 && length<=16*1024*1024,"Invalid shader bundle length");std::vector<uint8_t> bytes(length);need(fread(bytes.data(),1,bytes.size(),f)==bytes.size(),"Shader bundle read failed");fclose(f);',
        'std::vector<uint8_t> bytes;need(mng_shader_read(key,".xgs",bytes) && bytes.size()>=32,"Missing or invalid genuine shader bundle");')
replace('FILE *f=fopen((std::string(cfg.shader_directory)+"/"+key+".xgs").c_str(),"rb");if(!f)return VK_NULL_HANDLE;\n    fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);if(n<32 || n>16*1024*1024){fclose(f);return VK_NULL_HANDLE;}std::vector<uint8_t> b(static_cast<size_t>(n));bool ok=fread(b.data(),1,b.size(),f)==b.size();fclose(f);if(!ok)return VK_NULL_HANDLE;',
        'std::vector<uint8_t> b;if(!mng_shader_read(key,".xgs",b) || b.size()<32)return VK_NULL_HANDLE;')
replace('need(n && n<=256ull*1024*1024,"Invalid or excessive buffer size");',
        'if(!n || n>256ull*1024*1024)xbg_fatalf("Invalid or excessive buffer size: bytes=%llu cached=%d",(unsigned long long)n,int(cached));')
# Mac maps RGB10A2SN vertex attributes to signed normalized 10:10:10:2.
# Vulkan A2B10G10R10 has R in the low ten bits and signed two-bit alpha.
replace('case 0x3e:return VK_FORMAT_A2B10G10R10_UINT_PACK32;',
        'case 0x3e:return VK_FORMAT_A2B10G10R10_UINT_PACK32;case 0x69:return VK_FORMAT_A2B10G10R10_SNORM_PACK32;')
# Diagnose oversized initial images before deciding how to stage subresources.
replace('Buffer staging=stream_buffer(linear.size(),linear.data(),linear.size(),false);for(auto &copy:copies)copy.bufferOffset+=staging.offset;',
        'if(linear.size()>(64u<<20))xbg_logf("Large texture upload serial=%llu target=%d format=%#x extent=%ux%ux%u levels=%u layers=%u bpp=%u decoded=%d source_bytes=%llu linear_bytes=%zu",(unsigned long long)t->serial,t->d.target,t->d.format,im.width,im.height,im.depth,im.levels,im.layers,im.bpp,int(im.decoded),(unsigned long long)t->L.size,linear.size());\n        Buffer staging=stream_buffer(linear.size(),linear.data(),linear.size(),false);for(auto &copy:copies)copy.bufferOffset+=staging.offset;')
# BOTW uses both pipeline endpoints. Keep the engine's query retirement and
# ordering; calibrate their clock below and select the correct Vulkan stage for TIMESTAMP_TOP.
replace('need(type==NVN_COUNTER_TYPE_TIMESTAMP || type==NVN_COUNTER_TYPE_SAMPLES_PASSED,"Unsupported pipeline statistics query")',
        'need(type==NVN_COUNTER_TYPE_TIMESTAMP || type==NVN_COUNTER_TYPE_TIMESTAMP_TOP || type==NVN_COUNTER_TYPE_SAMPLES_PASSED,"Unsupported pipeline statistics query")')
replace('++counter_ops[type==NVN_COUNTER_TYPE_TIMESTAMP?1:2]', '++counter_ops[type==NVN_COUNTER_TYPE_SAMPLES_PASSED?2:1]')
replace('if(type==NVN_COUNTER_TYPE_TIMESTAMP && !heavy_timestamps', 'if(type!=NVN_COUNTER_TYPE_SAMPLES_PASSED && !heavy_timestamps')
replace('vkCmdWriteTimestamp(ts.cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,r.ts,r.ts_index)',
        'vkCmdWriteTimestamp(ts.cmd,type==NVN_COUNTER_TYPE_TIMESTAMP_TOP?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,r.ts,r.ts_index)')
replace('vkCmdWriteTimestamp(c.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,r.ts,r.ts_index)',
        'vkCmdWriteTimestamp(c.command,type==NVN_COUNTER_TYPE_TIMESTAMP_TOP?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,r.ts,r.ts_index)')
replace('#include "xbg_game_internal.h"', '#include "xbg_game_internal.h"\n#include "'+str(adapter/'border_shader.h')+'"')
replace('extern "C" {', 'extern "C" {\n__attribute__((weak)) void mng_texture_swizzle(uint64_t,VkComponentMapping*);\nint mng_nvn_shader_key(const void*,uint64_t,char[17]);\nint mng_vk_custom_border_enabled();')
replace('extern "C" {', 'extern "C" {\nvoid mng_nvn_note_gpu_range(uint64_t,uint64_t);')
replace('out.written.push_back({{base,bytes},b});',
        'mng_nvn_note_gpu_range(base,bytes);out.written.push_back({{base,bytes},b});')
replace('out.written.push_back({{base,bytes},buf});',
        'mng_nvn_note_gpu_range(base,bytes);out.written.push_back({{base,bytes},buf});')
replace('pending_writebacks.push_back({dst,n,b,&s});',
        'mng_nvn_note_gpu_range(dst,n);pending_writebacks.push_back({dst,n,b,&s});')
replace('pending_reports.push_back(r);',
        'mng_nvn_note_gpu_range(r.addr,sizeof(NVNcounterData));pending_reports.push_back(r);')
# Pending-write additions invalidate exact negative range results. Retirement
# only removes hazards and therefore cannot invalidate a negative result.
replace('struct Commands {', '#include "'+str(adapter/'read_range_cache_engine.inc')+'"\nstruct Commands {')
replace('pending_writebacks.push_back({v.first.first,v.first.second,*v.second,slot});',
        'mng_read_cache::invalidate();pending_writebacks.push_back({v.first.first,v.first.second,*v.second,slot});')
replace('pending_writebacks.push_back({dst,n,b,&s});',
        'mng_read_cache::invalidate();pending_writebacks.push_back({dst,n,b,&s});')
replace('pending_reports.push_back(r);', 'mng_read_cache::invalidate();pending_reports.push_back(r);')
replace('if(resolving_pending)return;\n    for(auto &w:pending_writebacks)',
        'if(resolving_pending)return;\n    const bool check_reports=why!=1 && why!=2;\n    if(mng_read_cache::contains(address,bytes,check_reports))return;\n    for(auto &w:pending_writebacks)')
replace('if(why==1 || why==2)return;',
        'if(!check_reports){mng_read_cache::remember(address,bytes,false);return;}')
replace('++perf.why_access;++perf.access_by[7];resolve_pending();return;}\n}',
        '++perf.why_access;++perf.access_by[7];resolve_pending();return;}\n    mng_read_cache::remember(address,bytes,true);\n}')
replace('recording_events.push_back({addr,mode,v,&cur});',
        'mng_nvn_note_gpu_range(addr,sizeof(uint32_t));recording_events.push_back({addr,mode,v,&cur});')
replace('pending_events.push_back({addr,mode,v,&last});',
        'mng_nvn_note_gpu_range(addr,sizeof(uint32_t));pending_events.push_back({addr,mode,v,&last});')
replace('Shader *load_shader(const void *control){', 'Shader *load_shader_key(const std::string &key){')
replace('Shader *load_shader_key(const std::string &key){', '#include "'+str(adapter/'shader_archive_engine.inc')+'"\nShader *load_shader_key(const std::string &key){')
replace('std::string key=control_key(control);auto found=shaders.find(key);',
        'auto found=shaders.find(key);')
# Immutable CPU command copies are pinned by the caller through synchronous
# translation; registration must not acquire the Vulkan/renderer mutex.
replace('size_t command_available(const uint8_t *p){',
        'extern "C" size_t mng_nvn_command_available(const void*);\nsize_t command_available(const uint8_t *p){')
replace('auto it=pools.upper_bound(address);',
        'if(size_t bytes=mng_nvn_command_available(p)){cmd_range_lo=address;cmd_range_bytes=bytes;return bytes;}auto it=pools.upper_bound(address);')
replace('uint64_t begin=timing_clock();snapshot_reset();uint64_t budget=0;',
        'spatial::LiteTraceScope translation("NVN.Translate");spatial::LiteTrace::counter("NVN.Submission.Token",submitted+1);uint64_t begin=timing_clock();snapshot_reset();cmd_range_lo=0;cmd_range_bytes=0;uint64_t budget=0;')
# Two thread-clock samples per batch separate execution from elapsed time. No
# per-draw clock syscalls; counters share the enclosing translation and token.
replace('uint64_t begin=timing_clock();snapshot_reset();cmd_range_lo=0;',
        'const uint64_t thread_begin=mng_thread_cpu_ns();uint64_t begin=timing_clock();snapshot_reset();cmd_range_lo=0;')
replace('perf.submit_cpu_ns+=timing_clock()-begin;',
        'const uint64_t elapsed=timing_clock()-begin,thread_end=mng_thread_cpu_ns();perf.submit_cpu_ns+=elapsed;spatial::LiteTrace::counter("NVN.Translate.ElapsedNs",elapsed);spatial::LiteTrace::counter("NVN.Translate.ThreadCpuValid",thread_begin && thread_end>=thread_begin);if(thread_begin && thread_end>=thread_begin)spatial::LiteTrace::counter("NVN.Translate.ThreadCpuNs",thread_end-thread_begin);mng::port::performance_hint_report(mng::port::HintLane::Translate,elapsed);')
# Use the pinned renderer's target-fence path, matching MacNative's completion
# contract. The waiter pins its VkFence, drops the engine lock for the GPU wait,
# and publishes completion only after ordered retirement/writeback. Retain a
# legacy switch for A/B; reset it per session rather than leaking test settings.
replace('constexpr bool completion_wait_default=false;', 'constexpr bool completion_wait_default=true;')
replace('bool legacy_waitall=true;', 'bool legacy_waitall=false;')
replace('fold_completion_wait_stats();completion_wait_enabled=completion_wait_default;',
        '''fold_completion_wait_stats();completion_wait_enabled=completion_wait_default;
    legacy_waitall=false;
    if(const char *mode=getenv("MNG_LEGACY_WAITALL"))legacy_waitall=strcmp(mode,"0")!=0;''')
replace('VkResult r=vkWaitForFences(cfg.device,1,&f,VK_TRUE,remaining);',
        '''VkResult r;{spatial::LiteTraceScope gpu_wait("NVN.SyncWait.GpuUnlocked");
            spatial::LiteTrace::counter("NVN.SyncWait.GpuTarget",fence);
            r=vkWaitForFences(cfg.device,1,&f,VK_TRUE,remaining);}''')
# A poll may inspect/retire already completed work, but must not fall through
# into a forced submit/all-slots wait when unresolved CPU reports remain.
replace('if(completion_path && deadline.expired())return fence<=completed;\n    SiteScope site(SITE_LEGACY_WAIT);',
        'if(!legacy_waitall && !timeout)return 0;\n    if(completion_path && deadline.expired())return fence<=completed;\n    SiteScope site(SITE_LEGACY_WAIT);')
# Preserve the conservative dependencies while making their source observable.
# Zero currently also represents the frontend's tiled-cache action boundary.
replace('case OP_BARRIER:{Commands c;',
        'case OP_BARRIER:{spatial::LiteTrace::counter("NVN.Barrier.Bits",payload<uint32_t>(p,n));spatial::LiteTrace::counter("NVN.Barrier.OpenPass",open_pass.cmd?1:0);Commands c;')
replace('if(im.layout!=VK_IMAGE_LAYOUT_GENERAL || im.last_write>barrier_seq || in_open){',
        '''if(im.layout!=VK_IMAGE_LAYOUT_GENERAL || im.last_write>barrier_seq || in_open){
        spatial::LiteTrace::counter("NVN.ImageRead.Hazard",(im.layout!=VK_IMAGE_LAYOUT_GENERAL?1:0)|(im.last_write>barrier_seq?2:0)|(in_open?4:0));''')
replace('void prepare_read(Image &im,VkCommandBuffer cmd){',
        '''namespace mng_read_hazards {void read(Image&,const view_t*,uint32_t);}
void prepare_read(Image &im,VkCommandBuffer cmd,const view_t *view=nullptr,uint32_t flags=0){''')
replace('end_open_pass();if(im.layout!=VK_IMAGE_LAYOUT_GENERAL)im.transition(cmd,VK_IMAGE_LAYOUT_GENERAL);full_barrier(cmd);',
        'mng_read_hazards::read(im,view,flags);end_open_pass();if(im.layout!=VK_IMAGE_LAYOUT_GENERAL)im.transition(cmd,VK_IMAGE_LAYOUT_GENERAL);full_barrier(cmd);')
replace('prepare_read(im,commands.command);}',
        'prepare_read(im,commands.command,ti.record->has_view?&ti.record->view:nullptr,ti.flags);}')
replace('struct MemoImage{const tex_t *first;Image *second;const RegisteredTexture *record;uint64_t revision;};',
        'struct MemoImage{const tex_t *first;Image *second;const RegisteredTexture *record;uint64_t revision;uint32_t flags;};')
replace('desc_memo_images.push_back({it->second.tex,im,it->second.record,it->second.revision});',
        'desc_memo_images.push_back({it->second.tex,im,it->second.record,it->second.revision,b.flags});')
replace('desc_memo_images.push_back({&texture,&im,record,record->revision});',
        'desc_memo_images.push_back({&texture,&im,record,record->revision,b.flags});')
replace('prepare_read(*im,commands.command);if(desc_memo_recording)',
        'prepare_read(*im,commands.command,it->second.record->has_view?&it->second.record->view:nullptr,b.flags);if(desc_memo_recording)')
replace('prepare_read(im,commands.command));if(desc_memo_recording)',
        'prepare_read(im,commands.command,registered.has_view?&registered.view:nullptr,b.flags));if(desc_memo_recording)')
replace('} state;', '} state;\n#include "'+str(adapter/'read_hazard_engine.inc')+'"')
replace('void begin_pass(Pass &p,Commands &c){',
        'void begin_pass(Pass &p,Commands &c){mng_read_hazards::begin(p.images,c.draw);')
replace('if(!occlusion_batch_enabled)end_occlusion_query();c.finish();',
        'mng_read_hazards::draw();if(!occlusion_batch_enabled)end_occlusion_query();c.finish();')
text = '#include "'+str(adapter/'../runtime/host_thread.h')+'"\n'+text
replace('pthread_setname_np(pthread_self(),"dm-gfx");','mng_name_thread("dm-gfx");')
text = '#include "'+str(adapter/'../runtime/serial_handoff.h')+'"\n'+text
replace('bool gfx_async=false;', 'std::unique_ptr<mng::port::SerialHandoff> present_worker;\nuint64_t mng_last_present_ticket=0;\nvoid (*mng_present_wait_callback)()=nullptr;\nbool mng_present_source_commands=false,mng_present_transition=true,mng_present_transition_pending=false,mng_range_invalidate=true;\nbool gfx_async=false;')
replace('extern "C" int xbg_game_shutdown(){gfx_drain();',
        'extern "C" int xbg_game_shutdown(){present_worker.reset();gfx_drain();')
replace('std::map<uint64_t,std::shared_ptr<Image>> images;',
        'std::shared_ptr<Image> mng_present_pin;\nstd::map<uint64_t,std::shared_ptr<Image>> images;')
replace('static void xbg_present_texture_now(const win_t *window,const tex_t *texture){std::lock_guard<std::recursive_mutex> lock(mutex);preflight_cpu_reads();',
        '''static void xbg_present_texture_now(const win_t *window,const tex_t *texture){std::lock_guard<std::recursive_mutex> lock(mutex);
        spatial::LiteTraceScope prepare("Host.Present.Prepare");
        mng_reads::images_read(texture);''')
replace('need(im.gpu_written,"Game tried to present a texture never rendered");',
        'need(im.gpu_written,"Game tried to present a texture never rendered");need(!mng_present_pin,"Overlapping present ownership");mng_present_pin=im.shared_from_this();')
replace('Commands c;im.transition(c.command,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);c.finish();submit_slots(false);int r;',
        '''if(mng_present_source_commands && mng_present_transition){
            // The host already owns a present command buffer. Submit recorded
            // rendering, then place the source transition in that buffer: do
            // not acquire an NVN arena just to record one layout barrier.
            submit_slots(false);mng_present_transition_pending=true;
        }else{Commands c;im.transition(c.command,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);c.finish();submit_slots(false);}
        int r;''')
replace('if(r)xbg_fatalf("Game presentation failed: %d",r);',
        'need(!mng_present_transition_pending,"Host omitted present source transition");if(r)xbg_fatalf("Game presentation failed: %d",r);')
present_start=text.index('extern "C" void xbg_present_texture(const win_t *window,const tex_t *texture){')
present_end=text.index('\nextern "C" void xbg_legacy_register_texture',present_start)
text=text[:present_start]+'''extern "C" void mng_nvn_set_present_wait(void (*callback)()) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    need(!present_worker && !mng_present_pin,"Set present completion before rendering");
    mng_present_wait_callback=callback;
}
extern "C" void mng_nvn_set_present_source_commands(){
    std::lock_guard<std::recursive_mutex> lock(mutex);
    need(!present_worker && !mng_present_pin,"Set present source capability before rendering");
    mng_present_source_commands=true;
}
extern "C" void mng_nvn_prepare_present_source(VkCommandBuffer command,VkImage source){
    // Invoked synchronously by cfg.present under the engine lock. The callback
    // must submit this command buffer to cfg.queue before returning; subsequent
    // translation can then rely on both queue order and the updated layout.
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if(!mng_present_transition_pending)return;
    need(mng_present_pin && mng_present_pin->image==source,"Present source identity mismatch");
    spatial::LiteTraceScope trace("Host.Present.SourceTransition");
    mng_present_pin->transition(command,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    mng_present_transition_pending=false;
}
extern "C" uint64_t mng_nvn_present_ticket(){return mng_last_present_ticket;}
extern "C" void mng_nvn_wait_present(uint64_t ticket){
    if(ticket){need(bool(present_worker),"Missing present ticket owner");present_worker->wait(ticket);}
}
void mng_present_complete(){
    spatial::LiteTraceScope scope("Host.Present.CompleteUnlocked");
    if(mng_present_wait_callback)mng_present_wait_callback();
}
void mng_present_retire(){
    spatial::LiteTraceScope scope("Host.Present.Retire");
    mng_present_pin.reset();
}
extern "C" void xbg_present_texture(const win_t *window,const tex_t *texture){
    mng_last_present_ticket=0;
    need(!gfx_async,"Native present requires owned command translation");
    if(const char *enabled=getenv("MNG_PRESENT_WORKER");enabled && !strcmp(enabled,"1")) {
        if(!present_worker){present_worker=std::make_unique<mng::port::SerialHandoff>(mutex);xbg_logf("NVN presentation handoff enabled: bounded=1 completion=unlocked command_translation=synchronous");}
        const win_t w=*window;const tex_t t=*texture;
        spatial::LiteTraceScope scope("Host.Present.Handoff");
        mng_last_present_ticket=present_worker->submit(
            [w,t]{static thread_local bool named=false;if(!named){mng_name_thread("mng-present");named=true;}xbg_present_texture_now(&w,&t);},
            mng_present_complete,mng_present_retire);
        return;
    }
    xbg_present_texture_now(window,texture);
    mng_present_complete();
    {std::lock_guard<std::recursive_mutex> lock(mutex);mng_present_retire();}
}
''' + text[present_end:]
text = '#include "'+str(adapter/'../runtime/gpu_clock.h')+'"\n#include "'+str(adapter/'../runtime/host_clock.h')+'"\n'+text
replace('void write_report(const PendingReport &r){',
        '#include "'+str(adapter/'guest_clock_engine.inc')+'"\nvoid write_report(const PendingReport &r){')
replace('result.timestamp=uint64_t(double(value)*properties.limits.timestampPeriod*12.0/625.0);',
        'result.timestamp=mng_clock::ticks(value);')
replace('timestamp_bits=queues[cfg.queue_family].timestampValidBits;',
        'timestamp_bits=queues[cfg.queue_family].timestampValidBits;mng_clock::reports=0;mng_clock::calibrate();')
replace('need(initialized&&cfg.present,"Game presentation callback not installed");',
        'need(initialized&&cfg.present,"Game presentation callback not installed");if(mng_clock::refresh_needed()){complete_all();mng_clock::calibrate();}')
replace('std::map<uint64_t,std::shared_ptr<Image>> images;',
        '#include "'+str(adapter/'texture_probe_engine.inc')+'"\nstd::map<uint64_t,std::shared_ptr<Image>> images;')
replace('Buffer staging=stream_buffer(linear.size(),linear.data(),linear.size(),false);for(auto &copy:copies)copy.bufferOffset+=staging.offset;',
        'texture_upload_probe(*t,im,linear);\n        Buffer staging=stream_buffer(linear.size(),linear.data(),linear.size(),false);for(auto &copy:copies)copy.bufferOffset+=staging.offset;')
text = '#include "'+str(adapter/'shader_variants.h')+'"\n'+text
text = '#include "'+str(adapter/'render_record_vulkan.h')+'"\n'+text
# Replay render targets remain live after their source command handle retires.
replace('struct State {', 'extern "C" void mng_nvn_retain_render_targets(const p_set_rt*);\nstruct State {')
replace('case OP_SET_RT:state.rt=payload<p_set_rt>(p,n);break;',
        'case OP_SET_RT:{auto next=payload<p_set_rt>(p,n);if(memcmp(&state.rt,&next,sizeof next)){mng_feedback::targets_changed();last_pass.valid=false;}mng_nvn_retain_render_targets(&next);state.rt=next;break;}')
replace('cur_op=h.op;\n        switch(h.op){', '''cur_op=h.op;
        mng_submit_batch(h.op);
        if(mng_render_record_active())mng_record_nvn_command(stats.submissions.load()+1,pc,h.op,h.size,p,n);
        switch(h.op){''')
replace('void execute(const uint8_t *pc,unsigned depth,uint64_t &budget){',
        '#include "'+str(adapter/'render_record_engine.inc')+'"\nvoid execute(const uint8_t *pc,unsigned depth,uint64_t &budget){')
replace('command=s.cmd;++perf.ops;++s.ops;', 'command=s.cmd;++perf.ops;++s.ops;mng_render_record_gpu_label(command);')
replace('struct State {', 'struct ShaderFamily;\nstruct State {\n    ShaderFamily *shader_family[6]{};')
replace('std::map<uint64_t,std::array<Shader*,6>> program_cache;',
        '#include "'+str(adapter/'shader_variants_engine.inc')+'"\nstd::map<uint64_t,std::array<ShaderFamily*,6>> program_cache;')
replace('std::array<Shader*,6>', 'std::array<ShaderFamily*,6>', 4)
replace('load_shader(prog->sd[i].control)', 'load_shader_family(prog->sd[i].control,prog->sd[i].data)')
replace('state.shader[st]=nullptr;if(!a.prog)', 'state.shader_family[st]=nullptr,state.shader[st]=nullptr;if(!a.prog)')
replace('Shader *s=loaded[i];if(a.stages&(1u<<nvn_stage[s->stage]))state.shader[s->stage]=s;',
        'ShaderFamily *s=loaded[i];if(a.stages&(1u<<nvn_stage[s->stage])){state.shader_family[s->stage]=s;state.shader[s->stage]=s->single;}')
replace('void cpu_access(uint64_t address,uint64_t bytes,unsigned why){',
        'namespace mng_reads {bool active=false;uint64_t checked=0,resolved=0;}\nvoid cpu_access(uint64_t address,uint64_t bytes,unsigned why){')
replace('++perf.why_access;if(why<12)++perf.access_by[why];resolve_pending();return;',
        'need(!mng_reads::active,"CPU writeback overlap missed draw/dispatch preflight");++perf.why_access;if(why<12)++perf.access_by[why];resolve_pending();return;')
replace('++perf.why_access;++perf.access_by[7];resolve_pending();return;',
        'need(!mng_reads::active,"CPU report overlap missed draw/dispatch preflight");++perf.why_access;++perf.access_by[7];resolve_pending();return;')
replace('void draw(int prim,int first,int count,const p_draw_idx *indexed,uint32_t instances=1,uint32_t first_instance=0){',
        '#include "'+str(adapter/'read_preflight_engine.inc')+'"\nvoid draw(int prim,int first,int count,const p_draw_idx *indexed,uint32_t instances=1,uint32_t first_instance=0){')
replace('preflight_cpu_reads();CPU_SCOPE(Draw);', 'resolve_shader_variants(false);if(count>0 && (!indexed || indexed->instances>0) && instances)mng_reads::operation(false,indexed,count);mng_reads::Guard read_guard;CPU_SCOPE(Draw);')
replace('preflight_cpu_reads();CPU_SCOPE(Dispatch);', 'resolve_shader_variants(true);if(p.x>0 && p.y>0 && p.z>0)mng_reads::operation(true);mng_reads::Guard read_guard;CPU_SCOPE(Dispatch);')
replace('need(!read_preflight || (pending_writebacks.empty() && pending_reports.empty()),"Unresolved CPU reads at descriptor entry");',
        'need(!read_preflight || mng_reads::active || (pending_writebacks.empty() && pending_reports.empty()),"Unresolved CPU reads at descriptor entry");')
# This optional load-op heuristic reads mapped vertex/index data on the CPU.
# A pending GPU view cannot supply CPU-visible values until its fence completes.
replace('if(p.pass_noload && quad_indices.empty()', 'if(pending_writebacks.empty() && p.pass_noload && quad_indices.empty()')
replace('void clear_attachment(const p_clear_color *color,const p_clear_ds *depth,int scalar=0){preflight_cpu_reads();',
        'void clear_attachment(const p_clear_color *color,const p_clear_ds *depth,int scalar=0){mng_feedback::boundary();mng_reads::attachments();mng_reads::Guard read_guard;')
replace('void clear_texture(const p_clear_tex &input){preflight_cpu_reads();',
        'void clear_texture(const p_clear_tex &input){mng_reads::images_read(input.tex);mng_reads::Guard read_guard;')
replace('void copy_texture(const p_copy_t2t &p){preflight_cpu_reads();',
        'void copy_texture(const p_copy_t2t &p){mng_reads::images_read(p.src,p.dst);mng_reads::Guard read_guard;')
replace('void resolve_texture(const p_copy_t2t &p){preflight_cpu_reads();',
        'void resolve_texture(const p_copy_t2t &p){mng_reads::images_read(p.src,p.dst);mng_reads::Guard read_guard;')
replace('program_cache.clear();shaders.clear();', 'program_cache.clear();shader_families.clear();border_variants.clear();shaders.clear();')
replace('std::set<uint32_t> storage_writes;', 'std::vector<uint32_t> spirv;std::map<uint32_t,uint32_t> border_canonical;std::vector<xbg_shader_binding> border_sources;\n    std::set<uint32_t> storage_writes;')
replace('Shader *load_shader_key(const std::string &key){', 'bool border_emulation_enabled();\nShader *load_shader_key(const std::string &key){')
replace('load_t3=timing_clock();', 'if(border_emulation_enabled())p->spirv=words;load_t3=timing_clock();')
replace('std::map<uint64_t,std::array<ShaderFamily*,6>> program_cache;', '#include "'+str(adapter/'border_shader_engine.inc')+'"\nstd::map<uint64_t,std::array<ShaderFamily*,6>> program_cache;')
replace('resolve_shader_variants(false);', 'resolve_shader_variants(false);resolve_border_variants(false);')
replace('resolve_shader_variants(true);', 'resolve_shader_variants(true);resolve_border_variants(true);')
replace('if(img_cache_enabled && !capture.active && !provenance_file && b.source==XBG_SOURCE_TEXTURE',
        'if(img_cache_enabled && shader.border_canonical.empty() && !capture.active && !provenance_file && b.source==XBG_SOURCE_TEXTURE')
replace('else if(b.source==XBG_SOURCE_SUPPORT){', 'else if(b.source==MNG_SOURCE_BORDER){n=shader.border_sources.size()*16;buffer=out.keep(border_colors(shader,ns));data=buffer->mapped;copy=n;}else if(b.source==XBG_SOURCE_SUPPORT){')
replace('ii.sampler=sampler(parameters,im,', 'if(auto border=shader.border_canonical.find(b.binding);border!=shader.border_canonical.end())for(float &c:parameters.border)c=float(border->second);ii.sampler=sampler(parameters,im,')

replace('std::map<std::array<uint32_t,16>,VkSampler> sampler_cache;', 'std::map<std::array<uint32_t,21>,VkSampler> sampler_cache;')
replace('VkSamplerCreateInfo s{};int result=xbg_sampler_from_nvn(&input,&limits,&s);', '''VkSamplerCreateInfo s{};int result=xbg_sampler_from_nvn(&input,&limits,&s);
    VkSamplerCustomBorderColorCreateInfoEXT border{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    bool custom=result==-7 && mng_vk_custom_border_enabled();
    if(custom){
        need(!integer_border,"Integer custom border conversion is not implemented");
        memset(input.border,0,sizeof(input.border));result=xbg_sampler_from_nvn(&input,&limits,&s);
        if(!result){memcpy(border.customBorderColor.float32,p.border,sizeof(p.border));border.format=viewed;
            s.borderColor=VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;s.pNext=&border;}
    }''')
replace('need(s.pNext==nullptr,"Unexpected sampler extension chain");', 'need(s.pNext==nullptr || (custom && s.pNext==&border),"Unexpected sampler extension chain");')
replace('std::array<uint32_t,16> key{s.flags,', 'std::array<uint32_t,21> key{s.flags,')
replace('uint32_t(s.borderColor),s.unnormalizedCoordinates};',
        'uint32_t(s.borderColor),s.unnormalizedCoordinates,custom?bits(p.border[0]):0,custom?bits(p.border[1]):0,custom?bits(p.border[2]):0,custom?bits(p.border[3]):0,custom?uint32_t(viewed):0};')
# Vulkan 1.1 block-texel views expose one raw integer texel per BC block.
# BOTW uses this to write compressed render targets (also handled by Mac's
# auxiliary block texture path). Preserve the native compressed allocation.
replace('if(source.d.format>=0x41 && source.d.format<=0x50){need(xbg_bc_view_compatible(source.d.format,v->format),"BC texture view must retain compression and signedness");return format(v->format);}',
        '''if(source.d.format>=0x41 && source.d.format<=0x50){
            if(xbg_bc_view_compatible(source.d.format,v->format))return format(v->format);
            int bw,bh;int bytes=nvn_fmt_bpb(v->format,&bw,&bh);
            need((create.flags&VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT) && bw==1 && bh==1 && bytes==int(source.L.bpb),"BC raw view must have one texel per compression block");
            return format(v->format);
        }''')
replace('VkFormat vf=view_format(v);', '''VkFormat vf=view_format(v);
        if((create.flags&VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT) && v && v->set_format && !(v->format>=0x41 && v->format<=0x50))
            need(n==1 && ls==1,"Raw BC views require one mip and layer");''')
replace('p->usage=usage;if(p->aspects==VK_IMAGE_ASPECT_COLOR_BIT', '''if(!p->decoded && t->d.format>=0x41 && t->d.format<=0x50){
            ci.flags|=VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT|VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT|VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
            VkFormat raw=p->bpp==8?VK_FORMAT_R16G16B16A16_UINT:VK_FORMAT_R32G32B32A32_UINT;
            const auto &support=format_properties(raw);
            if(support.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)usage|=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            if((t->d.flags&4) && (support.optimalTilingFeatures&VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))usage|=VK_IMAGE_USAGE_STORAGE_BIT;
            ci.usage=usage;
        }
        p->usage=usage;if(p->aspects==VK_IMAGE_ASPECT_COLOR_BIT''')
replace('trace=(trace^t.serial^uint64_t(i))', '''if(t.d.format>=0x41 && t.d.format<=0x50 && v && v->set_format && !(v->format>=0x41 && v->format<=0x50)){
            int bw,bh,vbw,vbh;int bytes=nvn_fmt_bpb(t.d.format,&bw,&bh),vbytes=nvn_fmt_bpb(v->format,&vbw,&vbh);
            need(bytes && bytes==vbytes && vbw==1 && vbh==1,"Invalid BC block attachment view");
            auto &e=extents[i];need(e.base_level<t.d.levels && e.base_level<32,"BC attachment mip out of range");
            e.width=(std::max(1u,t.d.width>>e.base_level)+bw-1)/bw;e.height=(std::max(1u,t.d.height>>e.base_level)+bh-1)/bh;
            e.levels=e.level_count=1;e.base_level=0;
            static std::set<std::pair<int,int>> seen;if(seen.insert({t.d.format,v->format}).second)
                xbg_logf("NVN BC block attachment format=%#x view=%#x blocks=%ux%u",t.d.format,v->format,e.width,e.height);
        }
        trace=(trace^t.serial^uint64_t(i))''')
replace('Image &im=get_image(targets[slot],false);const view_t *view=selected[slot];',
        'Image &im=get_image(targets[slot],false);const view_t *view=selected[slot];texture_writer_probe(*targets[slot],view,state.shader[0]?state.shader[0]->key.c_str():"none",state.shader[4]?state.shader[4]->key.c_str():"none",stats.presents.load(),clear_only);')
# Images alias backing storage; component selectors belong to the registered
# texture/view, not whichever texture originally created the shared image.
replace('VkImageView view(const view_t *v,bool attachment=false,bool storage=false){',
        'VkImageView view(const view_t *v,bool attachment=false,bool storage=false,uint64_t sampled_serial=0,uint32_t shader_image_flags=0){')
# MacNative's tex_type_view adapts BOTW's 2D glyph atlas to the shader's
# 2D-array signature (and the reverse). Vulkan also requires the descriptor
# view type to match OpTypeImage. Keep the registered subresource/swizzle,
# and use the selected first layer only when narrowing an array to 2D.
replace('VkImageAspectFlags view_aspects=!attachment', '''if(!attachment && !storage && (shader_image_flags&(1u<<31)) &&
           ((shader_image_flags>>8)&7)==1 && !(shader_image_flags&(1u<<13)) &&
           samples==VK_SAMPLE_COUNT_1_BIT &&
           (type==VK_IMAGE_VIEW_TYPE_2D || type==VK_IMAGE_VIEW_TYPE_2D_ARRAY)){
            const auto required=shader_image_flags&(1u<<12)?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;
            if(type!=required){type=required;ls=1;}
        }
        VkImageAspectFlags view_aspects=!attachment''')
# One texture handle can be used by different shader image signatures. A
# cache hit must not bypass the required view-type adaptation above.
replace('uint64_t(b.type)|(uint64_t((b.flags>>16)&3)<<8)',
        'uint64_t(b.type)|(uint64_t(b.flags)<<32)')
replace('struct Pass {', '#include "'+str(adapter/'attachment_alias_engine.inc')+'"\nstruct Pass {')
replace('struct LastPass{decltype(state.rt) rt;Pass pass;bool valid=false;}',
        'struct LastPass{decltype(state.rt) rt;Pass pass;bool valid=false;uint32_t colors=0;}')
replace('if(plain_draw && last_pass.valid && open_pass.cmd==c.command',
        'const uint32_t attached_colors=c.draw && clear_color<0 && !clear_depth && !soft?draw_attachment_mask():511;\n    if(plain_draw && last_pass.valid && last_pass.colors==attached_colors && open_pass.cmd==c.command')
replace('for(unsigned slot=0;slot<9;slot++)if(targets[slot]){',
        'for(unsigned slot=0;slot<9;slot++)if(targets[slot] && (attached_colors&(1u<<slot))){')
replace('last_pass.rt=state.rt;last_pass.pass=p;last_pass.valid=true;',
        'last_pass.rt=state.rt;last_pass.pass=p;last_pass.valid=true;last_pass.colors=attached_colors;')
replace('void begin_pass(Pass &p,Commands &c){',
        '''uint64_t mng_pass_begin_count=0;
extern "C" uint64_t mng_nvn_render_pass_begins(){std::lock_guard<std::recursive_mutex> lock(mutex);return mng_pass_begin_count;}
void begin_pass(Pass &p,Commands &c){++mng_pass_begin_count;''')
replace('if(state.rt.color[i] && (!clear_only || clear_color==i)){targets[i]',
        'if(state.rt.color[i] && (clear_only?clear_color==i:duplicate_color_slot(state.rt,i)<0)){targets[i]')
replace('uint32_t active_colors=0;for(int i=0;i<state.rt.n;i++)if(state.rt.color[i])',
        'validate_duplicate_color_writes();\n    uint32_t active_colors=0;for(int i=0;i<state.rt.n;i++)if(state.rt.color[i] && duplicate_color_slot(state.rt,i)<0)')
# Clears use the canonical slot even when its draw write mask is zero. The
# dedicated masked-clear path still controls its own channel mask.
replace('VkClearAttachment a{};uint32_t cm=0,sm=0;', '''p_clear_color canonical{};
    if(color){const int first=duplicate_color_slot(state.rt,color->index);
        if(first>=0){canonical=*color;canonical.index=first;color=&canonical;}}
    VkClearAttachment a{};uint32_t cm=0,sm=0;''')
replace('std::map<std::array<uint32_t,8>,VkImageView> views;',
        'std::map<std::array<uint32_t,12>,VkImageView> views;')
replace('std::array<uint32_t,8> key{base,n,layer,ls,uint32_t(vf),uint32_t(type),view_aspects,view_usage};auto it=views.find(key);if(it!=views.end())return it->second;', '')
replace('if(!attachment && !storage && (view_code==0x37 || view_code==0x6d))vi.components.a=VK_COMPONENT_SWIZZLE_ONE;', '''if(!attachment && !storage){
            vi.components={VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_G,VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_A};
            if(mng_texture_swizzle)mng_texture_swizzle(sampled_serial?sampled_serial:source.serial,&vi.components);
            // RGBX has an implicit one in A before the user mapping is applied.
            if(view_code==0x37 || view_code==0x6d){
                for(auto *component:{&vi.components.r,&vi.components.g,&vi.components.b,&vi.components.a})
                    if(*component==VK_COMPONENT_SWIZZLE_A)*component=VK_COMPONENT_SWIZZLE_ONE;
            }
        }
        std::array<uint32_t,12> key{base,n,layer,ls,uint32_t(vf),uint32_t(type),view_aspects,view_usage,
            uint32_t(vi.components.r),uint32_t(vi.components.g),uint32_t(vi.components.b),uint32_t(vi.components.a)};
        auto it=views.find(key);if(it!=views.end())return it->second;''')
replace('im.view(registered.has_view?&registered.view:nullptr,false,b.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)',
        'im.view(registered.has_view?&registered.view:nullptr,false,b.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,texture.serial,b.flags)')
# Include depth clamp in both the pipeline create-info and memo key.
replace('xbg_depth_bounds bounds{0,0,1};bool count_samples=false;', 'xbg_depth_bounds bounds{0,0,1};bool depth_clamp=false;float line_width=1;bool raster_discard=false;bool count_samples=false;')
replace('rs.polygonMode=state.poly.mode', 'rs.depthClampEnable=state.depth_clamp;need(!state.depth_clamp || cfg.enabled_core.depthClamp,"Depth clamp feature disabled");rs.polygonMode=state.poly.mode')
replace('rs.lineWidth=1;rs.depthBiasEnable=state.poly.offset_en!=0;', 'rs.lineWidth=state.line_width;rs.depthBiasEnable=state.poly.offset_en!=0;')
# Like the Metal encoder, keep per-draw scalar state out of pipeline identity.
# These are core Vulkan dynamic states; no optional driver extension is needed.
replace('VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=2;dy.pDynamicStates=dynamic;',
        '''const float dynamic_bias[]{rs.depthBiasConstantFactor,rs.depthBiasClamp,rs.depthBiasSlopeFactor};
    rs.depthBiasConstantFactor=rs.depthBiasClamp=rs.depthBiasSlopeFactor=0;rs.lineWidth=1;
    memset(blend.blendConstants,0,sizeof blend.blendConstants);
    VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_DEPTH_BIAS,VK_DYNAMIC_STATE_LINE_WIDTH,VK_DYNAMIC_STATE_BLEND_CONSTANTS};VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dy.dynamicStateCount=5;dy.pDynamicStates=dynamic;''')
replace('VkRect2D sc=scissor(p);', '''vkCmdSetDepthBias(c.command,dynamic_bias[0],dynamic_bias[1],dynamic_bias[2]);
    vkCmdSetLineWidth(c.command,state.line_width);vkCmdSetBlendConstants(c.command,state.blend_color);
    VkRect2D sc=scissor(p);''')
# The fast and previous-draw caches used to bypass LRU refresh, making the
# hottest pipelines look oldest as soon as the 4096-object limit was reached.
replace('uint64_t pipeline_tick=0;', '''uint64_t pipeline_tick=0;
std::unordered_map<VkPipeline,PipelineEntry*> pipeline_entries;
void touch_pipeline(VkPipeline pipeline){auto found=pipeline_entries.find(pipeline);
    need(found!=pipeline_entries.end(),"Pipeline fast cache lost its owner");found->second->last_used=++pipeline_tick;}''')
replace('constexpr size_t pipeline_object_limit=4096;', '''const size_t pipeline_object_limit=[]()->size_t{
    const char* value=getenv("MNG_PIPELINE_CACHE_ENTRIES");if(!value)return 4096;
    char* end=nullptr;unsigned long n=strtoul(value,&end,10);
    need(*value && end && !*end && n>=4 && n<=65536,"Pipeline cache entry limit must be 4..65536");return size_t(n);
}();''')
replace('void report_pipeline_cache(){', '''extern "C" uint64_t mng_nvn_pipeline_misses(){
    std::lock_guard<std::recursive_mutex> lock(mutex);return pipeline_counts.misses;
}
void report_pipeline_cache(){''')
replace('pipeline.pipeline=pipe_memo_pipeline;++pipe_memo_hits;', 'pipeline.pipeline=pipe_memo_pipeline;touch_pipeline(pipeline.pipeline);++pipe_memo_hits;')
replace('pipeline.pipeline=f->second;++pipeline_fast_hits;', 'pipeline.pipeline=f->second;touch_pipeline(pipeline.pipeline);++pipeline_fast_hits;')
replace('pipeline_objects.erase(oldest);++pipeline_counts.evictions;', 'pipeline_entries.erase(oldest->second.pipeline);pipeline_objects.erase(oldest);++pipeline_counts.evictions;')
replace('pipeline_objects.emplace(std::string(bytes),PipelineEntry{out.pipeline,++pipeline_tick});',
        'auto entry=pipeline_objects.emplace(std::string(bytes),PipelineEntry{out.pipeline,++pipeline_tick});pipeline_entries[out.pipeline]=&entry.first->second;')
replace('pipeline_objects.clear();pipeline_fast.clear();pipe_memo_valid=false;', 'pipeline_entries.clear();pipeline_objects.clear();pipeline_fast.clear();pipe_memo_valid=false;')
# Preserve previous lists, but do not eagerly rebuild their static scalar-state
# combinations after switching the pipeline key to dynamic state. The opaque
# driver pipeline cache remains valid and can still accelerate the new keys.
replace('"/vk-pipeline-list.bin"', '"/vk-pipeline-list-mng-dynamic-v1.bin"')
replace('"/vk-pipeline-list.seed"', '"/vk-pipeline-list-mng-dynamic-v1.seed"')
replace('rs.polygonMode=state.poly.mode', 'rs.rasterizerDiscardEnable=state.raster_discard;rs.polygonMode=state.poly.mode')
# Discarding rasterization still executes vertex/geometry side effects. Keep the
# draw, disable the full-cover load-op optimization, and permit a missing FS.
replace('xbg_graphics_stages(modules,state.rt.depth!=nullptr,active_colors,state.mask.mask,state.ms.a2c,',
        'xbg_graphics_stages(modules,state.raster_discard || state.rt.depth!=nullptr,state.raster_discard?0:active_colors,state.mask.mask,state.raster_discard?0:state.ms.a2c,')
replace('if(!vs || !fs || state.shader[1]', 'if(state.raster_discard || !vs || !fs || state.shader[1]')
replace('case OP_COUNT_:{', '''case OP_COUNT_+10:state.depth_clamp=payload<uint32_t>(p,n)!=0;break;
        case OP_COUNT_+12:state.raster_discard=payload<uint32_t>(p,n)!=0;break;
        case OP_COUNT_+11:{float width=payload<float>(p,n);need(width==1 || (cfg.enabled_core.wideLines && width>=properties.limits.lineWidthRange[0] && width<=properties.limits.lineWidthRange[1]),"Requested line width unsupported by Vulkan device");state.line_width=width;break;}
        case OP_COUNT_:{''')
replace('VkImageView result;VK(vkCreateImageView(cfg.device,&vi,nullptr,&result));views[key]=result;return result;',
        'static unsigned view_diagnostics=0;if(getenv("MNG_IMAGE_VIEW_DIAGNOSTICS") && view_diagnostics++<4096)xbg_logf("Image view serial=%llu parent_format=%u view_format=%u type=%u extent=%ux%ux%u mip=%u+%u layer=%u+%u usage=%#x",(unsigned long long)source.serial,fmt,vf,type,width,height,depth,base,n,layer,ls,view_usage);VkImageView result;VK(vkCreateImageView(cfg.device,&vi,nullptr,&result));views[key]=result;return result;')
# Keep unsupported image combinations actionable across Vulkan implementations.
replace('VkImageFormatProperties supported;VK(vkGetPhysicalDeviceImageFormatProperties(cfg.physical,ci.format,ci.imageType,ci.tiling,ci.usage,ci.flags,&supported));',
        'VkImageFormatProperties supported;auto format_result=vkGetPhysicalDeviceImageFormatProperties(cfg.physical,ci.format,ci.imageType,ci.tiling,ci.usage,ci.flags,&supported);if(format_result!=VK_SUCCESS)xbg_logf("Unsupported image: format=%u type=%u usage=%#x flags=%#x size=%ux%ux%u layers=%u result=%d",ci.format,ci.imageType,ci.usage,ci.flags,ci.extent.width,ci.extent.height,ci.extent.depth,ci.arrayLayers,format_result);VK(format_result);')
# BOTW renders individual depth slices of 3D textures as color attachments.
# Vulkan 1.1 maintenance1 permits 2D attachment views when the image opts in.
replace('if(t->d.target==8 || t->d.target==9)ci.flags|=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;',
        'if(t->d.target==8 || t->d.target==9)ci.flags|=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;\n        if(t->d.target==2)ci.flags|=VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;')
replace('need(base<levels && n && n<=levels-base && layer<layers && ls && ls<=layers-layer,"Invalid texture view range");',
        'const uint32_t view_layers=attachment && source.d.target==2 && base<32?std::max(1u,depth>>base):layers;\n        need(base<levels && n && n<=levels-base && layer<view_layers && ls && ls<=view_layers-layer,"Invalid texture view range");')
replace('need(!attachment || source.d.target!=2,"Rendering directly to volume slices requires 2D compatible views");',
        'need(!attachment || source.d.target!=2 || (create.flags&VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT),"Volume image lacks slice attachment support");')
replace('extents[i]={1,t.d.width,t.d.height,t.d.levels,t.L.layers,',
        'const uint32_t slice_mip=v&&v->set_levels?uint32_t(v->base_level):0;\n        const uint32_t attachment_layers=t.d.target==2 && slice_mip<32?std::max(1u,t.d.depth>>slice_mip):t.L.layers;\n        extents[i]={1,t.d.width,t.d.height,t.d.levels,attachment_layers,')
# Clear every selected array layer, including masked/scissored clears, through
# the existing single-layer path. General layered draws remain explicit errors
# until shader Layer outputs and multi-attachment coverage are implemented.
replace('void clear_attachment(const p_clear_color *color,const p_clear_ds *depth,int scalar=0){',
        'void clear_attachment_single(const p_clear_color *color,const p_clear_ds *depth,int scalar=0){')
replace('void region_check(const Image &im,', '''void clear_attachment(const p_clear_color *color,const p_clear_ds *depth,int scalar=0){
    need(color || depth,"Clear attachment arguments missing");
    if(color)need(color->index>=0 && color->index<state.rt.n && color->index<8,"Clear attachment index out of range");
    const tex_t *target=color?state.rt.color[color->index]:state.rt.depth;
    need(target,"Clear attachment missing");
    const view_t *view=color?(state.rt.has_cview[color->index]?&state.rt.cview[color->index]:nullptr):
        (state.rt.has_dview?&state.rt.dview:nullptr);
    uint32_t level=view && view->set_levels?view->base_level:0;
    need(level<target->d.levels && level<32,"Clear attachment mip out of range");
    uint32_t total=target->d.target==2?std::max(1u,target->d.depth>>level):target->L.layers;
    uint32_t first=view && view->set_layers?view->min_layer:0;
    uint32_t count=view && view->set_layers?view->nlayers:total;
    need(first<total && count && count<=total-first,"Clear attachment layers out of range");
    if(count==1){clear_attachment_single(color,depth,scalar);return;}
    const auto saved=state.rt;
    view_t selected=view?*view:view_t{};selected.set_layers=1;selected.nlayers=1;
    for(uint32_t layer=0;layer<count;++layer){
        selected.min_layer=first+layer;
        if(color){state.rt.has_cview[color->index]=1;state.rt.cview[color->index]=selected;}
        else{state.rt.has_dview=1;state.rt.dview=selected;}
        clear_attachment_single(color,depth,scalar);
    }
    state.rt=saved;
}
void region_check(const Image &im,''')
# Resolve quoted local includes against the original, validated source directory.
text = re.sub(r'^#include "([^"]+)"', lambda m: '#include "'+str((source.parent/m[1]).resolve())+'"', text, flags=re.M)
destination.parent.mkdir(parents=True, exist_ok=True)
# Genuine Foundation resident-ring CPU spans and asynchronous Vulkan queries.
text = '#include "profiler.h"\n' + text
replace('std::recursive_mutex mutex;', '''std::recursive_mutex mutex;
struct MngEngineLock {
    std::recursive_mutex &value;
    explicit MngEngineLock(std::recursive_mutex &m):value(m) {
        if(!value.try_lock()) {
            spatial::LiteTraceScope waiting("NVN.EngineLock.Wait");
            value.lock();
        }
    }
    ~MngEngineLock(){value.unlock();}
};''')
text = re.sub(r'std::lock_guard<std::recursive_mutex> (\w+)\(mutex\)',r'MngEngineLock \1(mutex)',text)
replace('#define CPU_SCOPE(kind) XbgCpuTiming::Scope cpu_scope(timing_now(),XbgCpuTiming::kind)',
        '#define CPU_SCOPE(kind) spatial::LiteTraceScope ring_scope;if(mng::profile::detailedCpu() || XbgCpuTiming::kind==XbgCpuTiming::Other || XbgCpuTiming::kind==XbgCpuTiming::Draw || XbgCpuTiming::kind==XbgCpuTiming::Dispatch || XbgCpuTiming::kind==XbgCpuTiming::Fence || XbgCpuTiming::kind==XbgCpuTiming::Submit || XbgCpuTiming::kind==XbgCpuTiming::Present || XbgCpuTiming::kind==XbgCpuTiming::CpuWrite)ring_scope.begin("NVN." #kind);XbgCpuTiming::Scope cpu_scope(timing_now(),XbgCpuTiming::kind)')
replace('struct Slot {', 'struct Slot {\n    mng::profile::GpuZone ring_zone{nullptr};')
replace('bool xview=false;', 'bool xview=false;uint64_t mng_submission_id=0;')
replace('{CPU_SCOPE(Submit);VK(vkQueueSubmit(cfg.queue,1,&si,s.fence));}',
        '''{CPU_SCOPE(Submit);s.mng_submission_id=slot_serial;
        spatial::LiteTrace::counter("NVN.Slot.SubmitId",s.mng_submission_id);
        spatial::LiteTrace::counter("NVN.Slot.Index",slot_index);
        VK(vkQueueSubmit(cfg.queue,1,&si,s.fence));}''')
replace('{CPU_SCOPE(Fence);bool busy=',
        '''{CPU_SCOPE(Fence);
     static const char *names[]={"NVN.Fence.Other","NVN.Fence.RingReuse","NVN.Fence.Present","NVN.Fence.Event","NVN.Fence.Arena","NVN.Fence.GpuSync","NVN.Fence.Complete","NVN.Fence.Poll","NVN.Fence.LegacyWait"};
     static_assert(sizeof(names)/sizeof(*names)==SITE_COUNT);
     spatial::LiteTraceScope cause(names[sync_site]);
     spatial::LiteTrace::counter("NVN.Fence.SubmitId",s.mng_submission_id);
     bool busy=''')
replace('VK(vkBeginCommandBuffer(s.cmd,&b));',
        'VK(vkBeginCommandBuffer(s.cmd,&b));mng::profile::collectGpu(s.cmd);mng::profile::beginGpu(s.ring_zone,s.cmd,"NVN.Submission");')
replace('VK(vkEndCommandBuffer(s.cmd));',
        'mng::profile::endGpu(s.ring_zone);mng::profile::collectGpu(s.cmd);VK(vkEndCommandBuffer(s.cmd));')
replace('VK(vkBeginCommandBuffer(s.pro,&pb));',
        'VK(vkBeginCommandBuffer(s.pro,&pb));mng::profile::GpuZone pro_zone(s.pro);mng::profile::beginGpu(pro_zone,s.pro,"NVN.UploadPrologue");')
replace('VK(vkEndCommandBuffer(s.pro));',
        'mng::profile::endGpu(pro_zone);VK(vkEndCommandBuffer(s.pro));')
replace('if(r!=VK_SUCCESS)return r;wtrack::release();',
        'if(r!=VK_SUCCESS)return r;mng::profile::shutdownGpu();wtrack::release();')
# Diagnose the first input of a matching attachment before descriptors import it.
replace('void draw(int prim,int first,int count,const p_draw_idx *indexed,uint32_t instances=1,uint32_t first_instance=0){',
        '#include "'+str(adapter/'texture_source_probe_engine.inc')+'"\nvoid draw(int prim,int first,int count,const p_draw_idx *indexed,uint32_t instances=1,uint32_t first_instance=0){texture_source_probe();')
# Match MacNative's separate serial cache and storage ownership. Finalizing a
# by-value texture removes an identity, not the GPU-written allocation. Storage
# survives temporary aliases, retires with its pool, and sweeps after 600 idle
# submissions with no cached identities. All destruction stays fence-deferred.
replace('struct Image {', 'struct Image : std::enable_shared_from_this<Image> {\n    uint64_t storage_last_use=0;')
replace('std::map<uint64_t,std::shared_ptr<Image>> images;',
        'uint64_t mng_image_index_rebuilds=0;\nstd::map<uint64_t,std::shared_ptr<Image>> images;\nstruct ImageStorage {std::shared_ptr<Image> owner;size_t serials;};\nstd::map<Image*,ImageStorage> image_storage;')
replace('image_index.clear();image_index_maxlen=0;\n    for(auto &e:images){uint64_t b=uintptr_t(e.second->source.mem);if(!b)continue;uint64_t n=e.second->source.L.size;image_index.push_back({b,b+n,e.second.get()});image_index_maxlen=std::max(image_index_maxlen,n);}',
        '''spatial::LiteTraceScope trace("NVN.ImageIndex.Rebuild");++mng_image_index_rebuilds;image_index.clear();image_index_maxlen=0;
    for(const auto &[image,storage]:image_storage){
        const uint64_t b=uintptr_t(image->source.mem);if(!b)continue;
        const uint64_t n=image->source.L.size;
        image_index.push_back({b,b+n,image});image_index_maxlen=std::max(image_index_maxlen,n);
    }''')
replace('if(alias)for(auto &candidate:images)if(candidate.second.get()==alias){it=images.emplace(t->serial,candidate.second).first;image_index_dirty=true;++memo_check_gen;++memo_bump[3];provenance_image("ALIAS",t,*it->second);break;}',
        'if(alias){it=images.emplace(t->serial,alias->shared_from_this()).first;++image_storage.at(alias).serials;++memo_check_gen;++memo_bump[3];provenance_image("ALIAS",t,*it->second);}')
replace('it=images.emplace(t->serial,std::move(p)).first;image_index_dirty=true;',
        'it=images.emplace(t->serial,std::move(p)).first;image_storage.emplace(it->second.get(),ImageStorage{it->second,1});image_index_dirty=true;')
replace('im.slot_use=slot_serial;', 'im.storage_last_use=stats.submissions;im.slot_use=slot_serial;', 2)
replace('auto i=images.find(t->serial);if(i!=images.end()){provenance_image("RELEASE",t,*i->second);retire_object(i->second);++memo_bump[2];}images.erase(t->serial);image_index_dirty=true;',
        '''auto i=images.lower_bound(t->serial & ~uint64_t(0xfff));
        while(i!=images.end() && (i->first>>12)==(t->serial>>12)){
            provenance_image("RELEASE",t,*i->second);
            auto &storage=image_storage.at(i->second.get());need(storage.serials,"Missing image identity");
            if(!--storage.serials)i->second->storage_last_use=stats.submissions;
            ++memo_bump[2];i=images.erase(i);
        }''')
replace('images.clear();++images_epoch;', 'image_storage.clear();images.clear();++images_epoch;')
# A surviving by-value copy may have no serial-cache entry. CPU readback must
# consult storage too; falling directly back to guest RAM loses GPU writes.
replace('auto found=images.find(t->serial);if(found==images.end() || !found->second->gpu_written)',
        '''auto found=images.find(t->serial);
        if(found==images.end() && t->mem){
            bool resident=false;for_images_overlapping(uintptr_t(t->mem),1,[&](Image &candidate){
                if(candidate.source.mem==t->mem && !memcmp(&candidate.source.d,&t->d,sizeof(t->d)))resident=true;
            });
            if(resident){get_image(t,false);found=images.find(t->serial);}
        }
        if(found==images.end() || !found->second->gpu_written)''')
replace('static void xbg_legacy_pool_add_now(', '#include "'+str(adapter/'image_storage_engine.inc')+'"\nstatic void xbg_legacy_pool_add_now(')
replace('if(pit!=pools.end())vcache::drop_pool(uintptr_t(p),pit->second);',
        'if(pit!=pools.end()){mng_storage_drop_pool(uintptr_t(p),pit->second);vcache::drop_pool(uintptr_t(p),pit->second);}')
replace('}}timing_boundary();}', '}}mng_storage_sweep();timing_boundary();}')
# Preserve linear render-target pitch semantics from MacNativeGames 3c482a7.
# Both flags independently select linear storage; the D2R baseline tested only
# LINEAR. Keep all input, copy and CPU readback decisions consistent.
replace('#include <algorithm>', '#include "'+str(adapter/'linear_texture.h')+'"\n#include <algorithm>')
replace('t->d.flags&0x10', 'mng_linear_texture(&t->d)', 3)
replace('p.tex->d.flags&0x10', 'mng_linear_texture(&p.tex->d)', 3)
replace('t->d.stride?t->d.stride:(size_t(w)*im.bpp+31)&~size_t(31)', 'mng_linear_stride(&t->d)')
replace('t->d.stride?t->d.stride:(w*im.bpp+31)&~size_t(31)', 'mng_linear_stride(&t->d)')
replace('need(!(mng_linear_texture(&t->d)),"Linear CPU ReadTexels not implemented");', '')
replace('need(nvn_deswizzle(&t->d,level,layer,t->mem,dst,row)==0,"ReadTexels deswizzle failed");',
        'if(!row)row=size_t((r->width+bw-1)/bw)*bpp;need(nvn_deswizzle(&t->d,level,layer,t->mem,dst,row)==0,"ReadTexels deswizzle failed");')
# Pair queries around actual Vulkan work, keeping optional probe coverage explicit.
# This pinned source uses statement calls; fail closed if the call surface changes.
gpu_sites = {}
def gpu_probe(match):
    function, arguments = match.group(1), match.group(2)
    gpu_sites[function] = gpu_sites.get(function, 0) + 1
    command = arguments.split(",", 1)[0]
    category = "Draw" if function.startswith("vkCmdDraw") else "Dispatch" if function == "vkCmdDispatch" else "Clear" if function.startswith("vkCmdClear") else "Blit" if function.startswith("vkCmdBlit") else "Copy"
    draw = "true" if category == "Draw" else "false"
    hazard = 'mng_read_hazards::clear();' if function == 'vkCmdClearAttachments' else ''
    return '{'+hazard+'mng::profile::GpuCommandScope gpu_scope('+command+',"GPU.'+category+'",'+draw+');'+match.group(0)+'}'
text = re.sub(r'\b(vkCmd(?:DrawIndexed|Draw|Dispatch|CopyBufferToImage|CopyImageToBuffer|CopyBuffer|CopyImage|ClearColorImage|ClearDepthStencilImage|ClearAttachments|BlitImage))\(([^;]*?)\);', gpu_probe, text)
if gpu_sites != {'vkCmdCopyBuffer':4,'vkCmdCopyBufferToImage':6,'vkCmdCopyImageToBuffer':4,
                 'vkCmdClearColorImage':2,'vkCmdClearDepthStencilImage':2,'vkCmdCopyImage':3,
                 'vkCmdDrawIndexed':1,'vkCmdDraw':2,'vkCmdDispatch':2,'vkCmdClearAttachments':4,'vkCmdBlitImage':2}:
    raise SystemExit("Pinned GPU probe call surface mismatch: " + repr(gpu_sites))
# Compile scopes wrap the driver calls themselves, not pipeline cache lookups.
text = re.sub(r'\b(vkCreate(?:Graphics|Compute)Pipelines)\(([^;]*?)\)',
              lambda m:'([&]{spatial::LiteTraceScope compile_scope("Host.Pipeline.Compile");return '+m.group(0)+';}())',text)
# Enumerate renderer-owned physical images, not historical guest texture objects.
# Keep the existing explicit-merge requirement for GPU-written mappings.
text += r'''
extern "C" void mng_invalidate_pool_images(const pool_t *pool) {
    gfx_drain();std::lock_guard<std::recursive_mutex> lock(mutex);
    for_images_overlapping(uintptr_t(pool->mem),pool->size,[](Image &image) {
        need(!image.gpu_written,"CPU invalidation of GPU-written texture needs explicit range merge");
        image.uploaded=false;
    });
}
extern "C" int mng_engine_resource_stats(uint64_t *identities,uint64_t *physical) {
    std::unique_lock lock(mutex,std::try_to_lock);if(!lock.owns_lock())return -1;
    *identities=images.size();*physical=image_storage.size();return 0;
}
extern "C" int mng_nvn_flush_snapshot(char *out,unsigned bytes){
    std::unique_lock lock(mutex,std::try_to_lock);if(!lock.owns_lock())return -1;
    MngFlushSample samples[32]{};const unsigned n=mng_nvn_flush_samples(samples,32);
    unsigned used=snprintf(out,bytes,"schema=mng.flush-ranges.v1\n");
    for(unsigned i=0;i<n && used+256<bytes;++i){auto s=samples[i];if(!s.count)continue;
        unsigned image_count=0,written=0,wb=0,reports=0;
        for_images_overlapping(s.address,s.bytes,[&](Image& im){++image_count;if(im.gpu_written)++written;});
        for(auto &w:pending_writebacks)if(xbg_wb_overlaps(s.address,s.bytes,w.addr,w.bytes))++wb;
        for(auto &r:pending_reports)if(xbg_wb_overlaps(s.address,s.bytes,r.addr,sizeof(NVNcounterData)))++reports;
        used+=snprintf(out+used,bytes-used,"range_%u=%llx,%llu,%llu,images:%u,gpu_written:%u,writebacks:%u,reports:%u\n",i,
            (unsigned long long)s.address,(unsigned long long)s.bytes,(unsigned long long)s.count,image_count,written,wb,reports);
    }return int(used);
}
'''
replace('struct LastPass{', '#include "'+str(adapter/'target_stats_engine.inc')+'"\nstruct LastPass{')
replace('void begin_pass(Pass &p,Commands &c){', 'void begin_pass(Pass &p,Commands &c){mng_targets::pass(p);')
replace('Image &im=get_image(texture,false);need(im.gpu_written,"Game tried to present a texture never rendered");',
        'Image &im=get_image(texture,false);need(im.gpu_written,"Game tried to present a texture never rendered");')
replace('if(r)xbg_fatalf("Game presentation failed: %d",r);++stats.presents;',
        'if(r)xbg_fatalf("Game presentation failed: %d",r);mng_targets::present(window,texture);++stats.presents;')
text += r'''
extern "C" int mng_nvn_target_snapshot(char *out,unsigned bytes) {
    std::unique_lock lock(mng_targets::cache_mutex,std::try_to_lock);if(!lock.owns_lock())return -1;
    return snprintf(out,bytes,"%s",mng_targets::cached.c_str());
}
extern "C" void mng_nvn_flush() {
    gfx_drain();std::lock_guard<std::recursive_mutex> lock(mutex);
    need(initialized,"Queue flush before Vulkan initialization");submit_slots(false);
}
extern "C" void mng_nvn_invalidate_range(uint64_t address,uint64_t bytes) {
    gfx_drain();std::lock_guard<std::recursive_mutex> lock(mutex);
    spatial::LiteTraceScope trace("NVN.Invalidate.Range");
    need(initialized,"CPU invalidation before Vulkan initialization");
    if(!mng_range_invalidate){complete_all();VK(vkQueueWaitIdle(cfg.queue));return;}
    // Events are CPU-applied at retirement, outside the writeback/report list.
    // Keep their established full ordering if the requested bytes intersect.
    for(const auto &e:pending_events)if(xbg_wb_overlaps(address,bytes,e.addr,4)){complete_all();return;}
    for(const auto &e:recording_events)if(xbg_wb_overlaps(address,bytes,e.addr,4)){complete_all();return;}
    cpu_access(address,bytes,8);
}
'''
text='#include "'+str(adapter/'../runtime/host_import.h')+'"\n'+text.replace('__attribute__((weak))','MNG_OPTIONAL_IMPORT')
# Legal sampled-attachment runs: snapshot before the run, preserve all
# explicit NVN synchronization, and keep descriptor cache identity distinct.
replace('struct Image;\nvoid bind_cache_reset();',
        'struct Image;\nnamespace mng_feedback {void boundary();void targets_changed();Image &sample(Image&);void key(std::vector<uint64_t>&);void begin_draw();}\nvoid bind_cache_reset();')
replace('struct Image : std::enable_shared_from_this<Image> {',
        'struct Image : std::enable_shared_from_this<Image> {\n    std::shared_ptr<Image> feedback_copy;uint64_t feedback_epoch=0;')
replace('if(!draw){end_open_pass();full_barrier(command);}',
        'if(!draw){mng_feedback::boundary();end_open_pass();full_barrier(command);}',2)
replace('uint64_t begin=timing_clock();snapshot_reset();cmd_range_lo=0;', 'uint64_t begin=timing_clock();mng_feedback::boundary();snapshot_reset();cmd_range_lo=0;')
replace('struct Shader {std::string key;', 'struct Shader {bool feedback_learned=false,feedback_storage=false;std::vector<uint16_t> feedback_slots;std::string key;')
replace('struct Descriptors {', '#include "'+str(adapter/'feedback_snapshot_engine.inc')+'"\nstruct Descriptors {')
replace('Commands c(true);Descriptors &d=acquire_descriptors();descriptors(d,c,false);',
        'mng_feedback::begin_draw();Commands c(true);Descriptors &d=acquire_descriptors();descriptors(d,c,false);')
replace('if(ok){CPU_SCOPE(Descriptor);',
        'if(ok){CPU_SCOPE(Descriptor);mng_feedback::key(key);if(tags)ktag.insert(ktag.end(),key.size(),13);')
replace('prepare_read(im,commands.command,ti.record->has_view?&ti.record->view:nullptr,ti.flags);}',
        'prepare_read(mng_feedback::sample(im),commands.command,ti.record->has_view?&ti.record->view:nullptr,ti.flags);}')
replace('struct ImgEntry{const tex_t *tex;Image *im;', 'struct ImgEntry{const tex_t *tex;Image *im;Image *view_owner;')
replace('live!=images.end() && live->second.get()==it->second.im)',
        'live!=images.end() && live->second.get()==it->second.im && it->second.view_owner==&mng_feedback::sample(*live->second))')
replace('prepare_read(*im,commands.command,it->second.record->has_view?',
        'prepare_read(mng_feedback::sample(*im),commands.command,it->second.record->has_view?')
replace('DESC_CALL(ImageTransition, prepare_read(im,commands.command,registered.has_view?',
        'Image &sampled=b.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE?im:mng_feedback::sample(im);DESC_CALL(ImageTransition, prepare_read(sampled,commands.command,registered.has_view?')
replace('DESC_CALL(ImageView, im.view(registered.has_view?', 'DESC_CALL(ImageView, sampled.view(registered.has_view?')
replace('ImgEntry{&texture,&im,ii.imageView,ii.sampler,record,record->revision}',
        'ImgEntry{&texture,&im,&sampled,ii.imageView,ii.sampler,record,record->revision}')
# Preserve dependencies while scheduling framebuffer-only work in one pass.
# OpCopyMemorySized is also a storage write, even when no OpStore follows.
replace('case 62:case 63:case 228:','case 62:case 63:case 64:case 228:')
replace('bool depth_attached=false,depth_used=false;} open_pass;',
        'bool depth_attached=false,depth_used=false,external_access=false,deferred_fragment_barrier=false;} open_pass;')
replace('void end_open_pass(){if(open_pass.cmd){', 'void full_barrier(VkCommandBuffer);\nvoid end_open_pass(){if(open_pass.cmd){')
replace('pass_prof_end(open_pass.cmd);open_pass=OpenPass{};',
        'pass_prof_end(open_pass.cmd);const auto ended=open_pass;open_pass=OpenPass{};if(ended.deferred_fragment_barrier)full_barrier(ended.cmd);')
replace('struct Shader {bool feedback_learned=',
        'struct Shader {bool barrier_metadata_known=false,barrier_external=false;bool feedback_learned=')
replace('struct Descriptors {', '#include "'+str(adapter/'fragment_barrier_engine.inc')+'"\nstruct Descriptors {')
replace('mng_feedback::begin_draw();Commands c(true);', 'mng_fragment_barrier::before_draw();mng_feedback::begin_draw();Commands c(true);')
replace('mng_read_hazards::draw();if(!occlusion_batch_enabled)', 'mng_fragment_barrier::after_draw();mng_read_hazards::draw();if(!occlusion_batch_enabled)')
replace('spatial::LiteTrace::counter("NVN.Barrier.OpenPass",open_pass.cmd?1:0);Commands c;',
        'spatial::LiteTrace::counter("NVN.Barrier.OpenPass",open_pass.cmd?1:0);if(mng_fragment_barrier::defer(payload<uint32_t>(p,n)))break;Commands c;')
# Host-only immutable metadata, derived once per exact state transition.
replace('struct Shader {bool barrier_metadata_known=',
        'struct Shader {bool uniform_counts_known=false;unsigned uniform_all=0,uniform_vary=0;bool barrier_metadata_known=')
replace('for(unsigned st=0;st<6;st++)if(state.shader[st] && (st==5)==compute)for(auto &b:state.shader[st]->bindings)if(b.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER){all+=b.count;if(b.source==XBG_SOURCE_UNIFORM||b.source==XBG_SOURCE_DRIVER0||b.source==XBG_SOURCE_DRIVER2)vary+=b.count;}',
        'for(unsigned st=0;st<6;st++)if(state.shader[st] && (st==5)==compute){auto &sh=*state.shader[st];if(!sh.uniform_counts_known){for(const auto &b:sh.bindings)if(b.type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER){sh.uniform_all+=b.count;if(b.source==XBG_SOURCE_UNIFORM||b.source==XBG_SOURCE_DRIVER0||b.source==XBG_SOURCE_DRIVER2)sh.uniform_vary+=b.count;}sh.uniform_counts_known=true;}all+=sh.uniform_all;vary+=sh.uniform_vary;}')
replace('void draw(int prim,int first,int count,const p_draw_idx *indexed,uint32_t instances=1,uint32_t first_instance=0){',
        '#include "'+str(adapter/'draw_plan_engine.inc')+'"\nvoid draw(int prim,int first,int count,const p_draw_idx *indexed,uint32_t instances=1,uint32_t first_instance=0){')
replace('static std::vector<VkVertexInputAttributeDescription> attributes;attributes.clear();VkVertexInputBindingDescription binding_slots[16];uint32_t binding_used=0;\n    int zero_binding=-1;',
        'auto &vertex_plan=mng_draw_plan::vertex;auto &attributes=vertex_plan.attributes;auto &bindings=vertex_plan.bindings;int &zero_binding=vertex_plan.zero_binding;\n    if(!vertex_plan.matches()){attributes.clear();VkVertexInputBindingDescription binding_slots[16];uint32_t binding_used=0;zero_binding=-1;')
replace('static std::vector<VkVertexInputBindingDescription> bindings;bindings.clear();for(uint32_t i=0;i<16;i++)if(binding_used&(1u<<i))bindings.push_back(binding_slots[i]);',
        'bindings.clear();for(uint32_t i=0;i<16;i++)if(binding_used&(1u<<i))bindings.push_back(binding_slots[i]);vertex_plan.remember();}')
replace('static std::vector<VkPipelineColorBlendAttachmentState> attachments;attachments.assign(size_t(state.rt.n),VkPipelineColorBlendAttachmentState{});',
        'auto &blend_plan=mng_draw_plan::blend;auto &attachments=blend_plan.attachments;if(!blend_plan.matches()){attachments.assign(size_t(state.rt.n),VkPipelineColorBlendAttachmentState{});')
replace('attachments[i]=convert.blend;}','attachments[i]=convert.blend;}blend_plan.remember();}')
replace('shaders.clear();textures.clear();','mng_draw_plan::reset();shaders.clear();textures.clear();')
# All non-draw dynamic state binders already invalidate BindCache. Keep the
# existing command-buffer and transient-pipeline invalidation rules.
replace('bool has_vp=false,has_sc=false;',
        'bool has_vp=false,has_sc=false,has_bias=false,has_width=false,has_blend=false;float bias[3]{},width{},blend[4]{};')
replace('vkCmdSetDepthBias(c.command,dynamic_bias[0],dynamic_bias[1],dynamic_bias[2]);\n    vkCmdSetLineWidth(c.command,state.line_width);vkCmdSetBlendConstants(c.command,state.blend_color);',
        'if(!bc.has_bias || memcmp(bc.bias,dynamic_bias,sizeof bc.bias)){vkCmdSetDepthBias(c.command,dynamic_bias[0],dynamic_bias[1],dynamic_bias[2]);memcpy(bc.bias,dynamic_bias,sizeof bc.bias);bc.has_bias=true;}else ++binds_skipped;\n    if(!bc.has_width || memcmp(&bc.width,&state.line_width,sizeof bc.width)){vkCmdSetLineWidth(c.command,state.line_width);bc.width=state.line_width;bc.has_width=true;}else ++binds_skipped;\n    if(!bc.has_blend || memcmp(bc.blend,state.blend_color,sizeof bc.blend)){vkCmdSetBlendConstants(c.command,state.blend_color);memcpy(bc.blend,state.blend_color,sizeof bc.blend);bc.has_blend=true;}else ++binds_skipped;')
# Record the actual reason for a pass boundary, separately from the NVN opcode
# that happened to trigger it. These counters never change synchronization.
replace('void end_open_pass(){if(open_pass.cmd){',
        'uint64_t pass_end_reasons[16]{};\nvoid end_open_pass(unsigned reason=0){if(open_pass.cmd){++pass_end_reasons[reason];spatial::LiteTrace::counter("NVN.Pass.EndReason",reason);')
replace('cur_op=60;end_open_pass();','cur_op=60;end_open_pass(1);')
replace('mng_feedback::boundary();end_open_pass();full_barrier(command);',
        'mng_feedback::boundary();end_open_pass(3);full_barrier(command);',2)
replace('mng_read_hazards::read(im,view,flags);end_open_pass();',
        'mng_read_hazards::read(im,view,flags);end_open_pass(4);')
replace('if(im.layout!=VK_IMAGE_LAYOUT_GENERAL && !in_open){end_open_pass();',
        'if(im.layout!=VK_IMAGE_LAYOUT_GENERAL && !in_open){end_open_pass(5);')
replace('VkCommandBuffer cmd=recording_slot().cmd;end_open_pass();VkBufferCopy',
        'VkCommandBuffer cmd=recording_slot().cmd;end_open_pass(6);VkBufferCopy',2)
replace('if(open_pass.cmd!=c.command || open_pass.pass!=p.pass || open_pass.framebuffer!=p.framebuffer){end_open_pass();',
        'if(open_pass.cmd!=c.command || open_pass.pass!=p.pass || open_pass.framebuffer!=p.framebuffer){end_open_pass(7);',2)
replace('cur_op=62;end_open_pass();','cur_op=62;end_open_pass(8);')
replace('Slot &s=recording_slot();end_open_pass();Buffer b=readback_buffer(n);',
        'Slot &s=recording_slot();end_open_pass(10);Buffer b=readback_buffer(n);')
replace('set_binds=set_bind_offsets=set_binds_offsets_only=0;',
        'xbg_logf("NVN PERF pass_reasons unknown=%.1f submit=%.1f command=%.1f attachment_read=%.1f layout=%.1f buffer_copy=%.1f targets=%.1f writeback=%.1f readback=%.1f feedback_copy=%.1f external=%.1f",pass_end_reasons[0]/f,pass_end_reasons[1]/f,pass_end_reasons[3]/f,pass_end_reasons[4]/f,pass_end_reasons[5]/f,pass_end_reasons[6]/f,pass_end_reasons[7]/f,pass_end_reasons[8]/f,pass_end_reasons[10]/f,pass_end_reasons[11]/f,pass_end_reasons[12]/f);for(auto &n:pass_end_reasons)n=0;\n    set_binds=set_bind_offsets=set_binds_offsets_only=0;')


# Preserve the preflight ordering, then use the existing epoch-checked image
# front cache. The original read_preflight branch always called the slow path,
# even after resolving the same texture range earlier in this draw.
replace('inline Image &get_image(const tex_t *t,bool upload){\n    if(read_preflight)return get_image_read(t,upload);',
        '#include "'+str(adapter/'translation_options.h')+'"\ninline Image &get_image_checked(const tex_t *t,bool upload){')
replace('Image &get_image_slow(const tex_t *t,bool upload){CPU_SCOPE(Image);',
        'inline Image &get_image(const tex_t *t,bool upload){return read_preflight?get_image_read(t,upload):get_image_checked(t,upload); }\nImage &get_image_slow(const tex_t *t,bool upload){CPU_SCOPE(Image);')
replace('[upload](const tex_t &stable)->Image &{return get_image_slow(&stable,upload);}',
        '[upload](const tex_t &stable)->Image &{return mng_translate::image_fast()?get_image_checked(&stable,upload):get_image_slow(&stable,upload);}')
# The cached descriptor's dynamic bindings already have canonical Vulkan order.
# Store the guest uniform source with that immutable plan. Hits refresh from
# current guest addresses, without rebuilding and sorting a second Buffer list.
replace('uint8_t src=0,ns=0,bank=0;};std::vector<Dyn>',
        'uint8_t src=0,ns=0,bank=0,slot=0;size_t copy=0;};std::vector<Dyn>')
replace('dyn_ubos.push_back({(uint64_t(b.binding)<<32)|element,r.address,copy});',
        'if(!mng_translate::descriptor_plan())dyn_ubos.push_back({(uint64_t(b.binding)<<32)|element,r.address,copy});')
replace('uint8_t(b.source==XBG_SOURCE_DRIVER0?0:1)});dynamic_list.push_back',
        'uint8_t(b.source==XBG_SOURCE_DRIVER0?0:1),uint8_t(slot),copy});dynamic_list.push_back')
replace('if(fi>=fresh.size() || fresh[fi].first!=d.order || fresh[fi].second.buffer!=d.buffer){same=false;break;}offsets.push_back(uint32_t(fresh[fi].second.offset));++fi;',
        'if(mng_translate::descriptor_plan()){const auto r=state.ub[d.ns][d.slot];Buffer b=uniform_snapshot(r.address,d.copy);if(b.buffer!=d.buffer){same=false;break;}offsets.push_back(uint32_t(b.offset));}else{if(fi>=fresh.size() || fresh[fi].first!=d.order || fresh[fi].second.buffer!=d.buffer){same=false;break;}offsets.push_back(uint32_t(fresh[fi].second.offset));++fi;}')

# Bounded whole-draw detail avoids flooding the resident trace with millions
# of tiny snapshot scopes. Encode is the ordered driver-call stage that could
# overlap preparation through immutable packets (Mac's render stream model).
replace('mng_reads::Guard read_guard;CPU_SCOPE(Draw);',
        'mng_reads::Guard read_guard;mng::profile::CpuDetailDrawScope detail_draw;CPU_SCOPE(Draw);spatial::LiteTraceScope detail_sample;if(mng::profile::detailedCpu())detail_sample.begin("NVN.Draw.Sample");')
replace('VkQueryPool occlusion{};uint32_t occlusion_index=0;',
        '{spatial::LiteTraceScope encode;if(mng::profile::detailedCpu())encode.begin("NVN.Draw.Encode");\n    VkQueryPool occlusion{};uint32_t occlusion_index=0;')
replace('pass_prof_verts+=uint64_t(uint32_t(count))*instances;',
        'pass_prof_verts+=uint64_t(uint32_t(count))*instances;}')

# A shared UMA allocation policy. Keep the resource's memoryTypeBits and
# coherent fallback; uploaded bytes still belong to the fence-retired arena.
text='#include "'+str(adapter/'memory_policy.h')+'"\n'+text
replace(':find_memory(r.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);',
        ':mng::vulkan::upload_memory_type(memories,r.memoryTypeBits,mng::vulkan::policy::upload_cached());need(type!=UINT32_MAX,"No host-visible upload memory type");')

text="#include <optional>\n"+text
# Check the validated previous-draw pipeline BEFORE building the Vulkan create
# infos. Resources/descriptors and Pass are still resolved on every draw. The
# exact existing key includes selected shader variants and target compatibility;
# all existing eviction/reset paths retain ownership of memo invalidation.
replace('auto &vertex_plan=mng_draw_plan::vertex;auto &attributes=vertex_plan.attributes;auto &bindings=vertex_plan.bindings;int &zero_binding=vertex_plan.zero_binding;',
        r'''PipelineLease pipeline;
    const bool use_memo_fast=pipeline_fast_enabled && !ab_off("pipefast") && d.entry->definition.valid && p.definition->valid;
    const bool memo=use_memo_fast && pipe_memo && !ab_off("pipememo");
    bool memo_checked=false;
    std::optional<spatial::LiteTraceScope> pipeline_prepare;if(mng::profile::detailedCpu())pipeline_prepare.emplace("NVN.Draw.PipelinePrepare");
    if(mng_translate::pipeline_early() && memo && !capture.active && mng_draw_plan::vertex.matches()){
        pipe_memo_fill(pipe_memo_next,&d.entry->definition,p.definition,p.pass,d.pipeline_layout,prim,p.samples,active_colors);memo_checked=true;
        if(pipe_memo_valid && !memcmp(&pipe_memo_next,&pipe_memo_key,sizeof pipe_memo_next)){
            pipeline.pipeline=pipe_memo_pipeline;touch_pipeline(pipeline.pipeline);++pipe_memo_hits;++mng_translate::pipeline_early_hits;
        }
    }
    const float dynamic_bias[]{state.bias[1]*(state.rt.depth && state.rt.depth->d.format==0x32 && depth16_as_d24?256.0f:1.0f),state.bias[2],state.bias[0]};
    auto &vertex_plan=mng_draw_plan::vertex;auto &attributes=vertex_plan.attributes;auto &bindings=vertex_plan.bindings;int &zero_binding=vertex_plan.zero_binding;
    if(!pipeline.pipeline){++mng_translate::pipeline_state_builds;''')
replace('const float dynamic_bias[]{rs.depthBiasConstantFactor,rs.depthBiasClamp,rs.depthBiasSlopeFactor};','')
replace('PipelineLease pipeline;{CPU_SCOPE(Pipeline);\n        static std::vector<uint32_t> fast;fast.clear();bool use_fast=pipeline_fast_enabled && !ab_off("pipefast") && d.entry->definition.valid && p.definition->valid;\n        bool memo=use_fast && pipe_memo && !ab_off("pipememo");',
        '{CPU_SCOPE(Pipeline);\n        static std::vector<uint32_t> fast;fast.clear();bool use_fast=use_memo_fast;')
replace('if(memo){pipe_memo_fill(pipe_memo_next,&d.entry->definition,p.definition,p.pass,d.pipeline_layout,prim,p.samples,active_colors);',
        'if(memo){if(!memo_checked)pipe_memo_fill(pipe_memo_next,&d.entry->definition,p.definition,p.pass,d.pipeline_layout,prim,p.samples,active_colors);')
replace('pipe_memo_pipeline=pipeline.pipeline;pipe_memo_valid=true;}}',
        'pipe_memo_pipeline=pipeline.pipeline;pipe_memo_valid=true;}}}pipeline_prepare.reset();')
replace('spatial::LiteTrace::counter("NVN.Translate.ElapsedNs",elapsed);',
        'spatial::LiteTrace::counter("NVN.Pipeline.EarlyHits",mng_translate::pipeline_early_hits);spatial::LiteTrace::counter("NVN.Pipeline.StateBuilds",mng_translate::pipeline_state_builds);spatial::LiteTrace::counter("NVN.Translate.ElapsedNs",elapsed);')

text += r'''
extern "C" int mng_pipeline_cache_probe(char *out,unsigned bytes){
    std::lock_guard<std::recursive_mutex> lock(mutex);
    snprintf(out,bytes,"early pipeline enabled=%u hits=%llu full-state builds=%llu",unsigned(mng_translate::pipeline_early()),
        (unsigned long long)mng_translate::pipeline_early_hits,(unsigned long long)mng_translate::pipeline_state_builds);
    return mng_translate::pipeline_early() && (!mng_translate::pipeline_early_hits || !mng_translate::pipeline_state_builds)?-1:0;
}
'''

from prepare_warm_jobs import adapt as adapt_warm_jobs
text=adapt_warm_jobs(text,adapter)
from prepare_preparation import adapt as adapt_preparation
text=adapt_preparation(text,adapter)
# Ordered translation keeps the engine's original GPU completion identities.
# Only the adapter owns the new queue; never enable D2R's borrowed-pointer queue.
text = '#include "'+str(adapter/'../runtime/submission_inputs.h')+'"\n'+text
replace('Buffer snapshot_slow(uint64_t address,size_t bytes,VkDeviceSize align);', '''thread_local const mng::port::SubmissionInputs *mng_submission_inputs=nullptr;
const uint8_t *mng_input(uint64_t address,size_t bytes){
    if(!mng_submission_inputs)return reinterpret_cast<const uint8_t*>(address);
    const auto *input=mng_submission_inputs->find(address,bytes);
    need(input,"Uncaptured ordered input");return static_cast<const uint8_t*>(input);
}
struct MngInputScope{
    MngInputScope(const mng::port::SubmissionInputs *p){need(!mng_submission_inputs,"Nested ordered translation");mng_submission_inputs=p;}
    ~MngInputScope(){mng_submission_inputs=nullptr;}
};
Buffer snapshot_slow(uint64_t address,size_t bytes,VkDeviceSize align);''')
# Retain the registered output generation past translation, until GPU retirement
# performs the existing writeback. No query, fence or CPU visibility is elided.
replace('bool ts_known;};', 'bool ts_known;std::shared_ptr<const mng::port::ReportPool> mng_output_pool;};')
replace('PendingReport r{addr,type,counter_epoch,epochs[counter_epoch].size(),nullptr,VK_NULL_HANDLE,0,0,false};',
        '''PendingReport r{addr,type,counter_epoch,epochs[counter_epoch].size(),nullptr,VK_NULL_HANDLE,0,0,false,{}};
            if(mng_submission_inputs){r.mng_output_pool=mng_submission_inputs->report_pool(addr);need(bool(r.mng_output_pool),"Unowned ordered report output");}''')
replace('void write_report(const PendingReport &r){',
        '''void write_report(const PendingReport &r){
    need(!r.mng_output_pool || r.mng_output_pool->contains(r.addr,sizeof(NVNcounterData)),"Expired ordered report output");''')
replace('if(snap_site==3 && bytes>=wtrack::min_bytes',
        'if(!mng_submission_inputs && snap_site==3 && bytes>=wtrack::min_bytes')
replace('if((snap_site==1 || snap_site==2) && vcache::enabled',
        'if(!mng_submission_inputs && (snap_site==1 || snap_site==2) && vcache::enabled')
replace('Buffer b=stream_buffer(bytes,reinterpret_cast<const void*>(address),bytes,false);',
        'Buffer b=stream_buffer(bytes,mng_input(address,bytes),bytes,false);')
replace('Buffer b=stream_buffer(n,reinterpret_cast<const void*>(address),copy,true);uniform_snapshots.emplace(key,b);',
        'Buffer b=stream_buffer(n,mng_input(address,copy),copy,true);uniform_snapshots.emplace(key,b);')
replace('const uint8_t *src=reinterpret_cast<const uint8_t*>(indexed->ibuf);',
        'const uint8_t *src=mng_input(indexed->ibuf,size_t(count));')
replace('memcpy(m,reinterpret_cast<const void*>(u.address),64);',
        'memcpy(m,mng_input(u.address,64),64);')
replace('memcpy(f,reinterpret_cast<const uint8_t*>(v.address)+20*i,8);',
        'memcpy(f,mng_input(v.address+20*i,8),8);')
replace('if(b.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER && texel_memo_enabled && !compute)',
        'if(b.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER && texel_memo_enabled && !compute && !mng_submission_inputs)')
# Keep image identities, alias keys and GPU-written state on original addresses.
# Only CPU hash/deswizzle/linear-copy inputs use the immutable submission bytes.
start=text.index('Image &get_image_slow(')
end=text.index('// read_preflight:',start)
image=text[start:end]
image=image.replace('Image &im=*hit;', 'Image &im=*hit;if(mng_submission_inputs){im.dirty_stamp=0;im.stable_checks=0;}')
image=image.replace('const bool track=tex_dirty', 'const bool track=!mng_submission_inputs && tex_dirty')
image=image.replace('if(upload && im.gpu_written && im.decoded', 'if(!mng_submission_inputs && upload && im.gpu_written && im.decoded')
image=image.replace('hash_memory(t->mem,t->L.size)', 'hash_memory(mng_input(uint64_t(t->mem),t->L.size),t->L.size)')
image=image.replace('level,layer,t->mem,', 'level,layer,mng_input(uint64_t(t->mem),t->L.size),')
image=image.replace('t->mem+y*row', 'mng_input(uint64_t(t->mem),t->L.size)+y*row')
image=image.replace('t->mem+y*pitch', 'mng_input(uint64_t(t->mem),t->L.size)+y*pitch')
text=text[:start]+image+text[end:]
replace('static void xbg_submit_now(int n,const uint64_t *handles){',
        '#include "'+str(adapter/'ordered_submission_engine.inc')+'"\nstatic void xbg_submit_now(int n,const uint64_t *handles){')
replace('[w,t]{static thread_local bool named=false;',
        '[w,t,input=mng_submission_inputs]{MngInputScope input_scope(input);static thread_local bool named=false;')
text += '''
extern "C" uint64_t mng_execute_submission(int count,const uint64_t *handles,const mng::port::SubmissionInputs *input){
    MngInputScope scope(input);
    xbg_submit_now(count,handles);
    return xbg_legacy_last_fence();
}
extern "C" void mng_execute_present(const win_t *window,const tex_t *texture,const mng::port::SubmissionInputs *input){
    MngInputScope scope(input);xbg_present_texture(window,texture);
    // SerialHandoff returns on lock acquisition, before CPU preparation ends.
    // Keep the packet alive until that prepare scope releases the engine lock.
    std::lock_guard<std::recursive_mutex> prepared(mutex);
}
'''
from prepare_live_flush import adapt as adapt_live_flush
text=adapt_live_flush(text,adapter)
destination.write_text(text)

texture_source=source.parent/"xbg_texture.c"
texture=texture_source.read_text()
old='pthread_setname_np(pthread_self(),"xbg-bcdec");'
if texture.count(old)!=1:raise SystemExit("Pinned BC worker patch mismatch")
texture=texture.replace(old,'mng_name_thread("xbg-bcdec");')
texture=re.sub(r'^#include "([^"]+)"',lambda m:'#include "'+str((texture_source.parent/m[1]).resolve())+'"',texture,flags=re.M)
texture='#include "'+str(adapter/'../runtime/host_thread.h')+'"\n#include "'+str(adapter/'../runtime/host_import.h')+'"\n'+texture.replace('__attribute__((weak))','MNG_OPTIONAL_IMPORT')
(destination.parent/"xbg_texture.c").write_text(texture)

# The native Zelda control section records its own byte size. BOTW has shorter
# sections than D2R; hashing a fixed 0x880 bytes includes unrelated allocation
# contents. Apply the same bounded-length rule as prepare_shader_seed.py.
key_source = source.parent / "xbg_shader_key.c"
key = key_source.read_text()
old = "uint8_t bytes[0x8c0]={0};memcpy(bytes,data,0x880);bytes[0x880]=0x80;bytes[sizeof(bytes)-2]=0x44; // 0x880 * 8 = 0x4400"
new = """uint32_t declared;memcpy(&declared,(const uint8_t*)data+0x20,4);
    size_t length=declared>=0x7cc && declared<0x880?declared:0x880;
    uint8_t bytes[0x8c0]={0};memcpy(bytes,data,length);bytes[length]=0x80;
    size_t padded=(length+9+63)&~(size_t)63;uint64_t bits=length*8;
    for(unsigned i=0;i<8;i++)bytes[padded-1-i]=(uint8_t)(bits>>(8*i));"""
if key.count(old)!=1:raise SystemExit("Pinned shader-key patch mismatch")
key=key.replace(old,new).replace("off<sizeof(bytes)","off<padded")
(destination.parent/"xbg_shader_key.c").write_text(key)

# Adapt build-local texture helpers without changing the pinned D2R checkout.
for relative, occurrences in [("nvn/nvn_tex.c", 2), ("tex/nvn_deswizzle.c", 1)]:
    original = source.parent.parent.parent / relative
    helper = original.read_text()
    if helper.count("d->flags & 0x10") != occurrences:
        raise SystemExit("Pinned texture layout patch mismatch: " + relative)
    helper = helper.replace("d->flags & 0x10", "d->flags & 0x50")
    if relative.startswith("tex/"):
        anchor = "    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;"
        linear = """    if (mng_linear_texture(d)) {
        if (level || layer || d->samples > 1 || (d->target != 0 && d->target != 1 && d->target != 7)) return -1;
        int bw, bh; int bpp = nvn_fmt_bpb(d->format, &bw, &bh);
        if (!bpp) return -1;
        size_t bytes = (size_t)((d->width + bw - 1) / bw) * bpp;
        size_t rows = d->target == 0 ? 1 : (d->height + bh - 1) / bh;
        size_t stride = mng_linear_stride(d);
        if (pitch < bytes || stride < bytes) return -1;
        for (size_t y = 0; y < rows; ++y) {
            if (to_linear) memcpy(lin + y * pitch, bl + y * stride, bytes);
            else memcpy(bl + y * stride, lin + y * pitch, bytes);
        }
        return 0;
    }
"""
        if helper.count(anchor) != 1: raise SystemExit("Pinned linear copy patch mismatch")
        helper = helper.replace(anchor, linear + anchor)
        helper = helper.replace('#include <string.h>', '#include <string.h>\n#include "'+str(adapter/'linear_texture.h')+'"')
    helper = re.sub(r'^#include "([^"]+)"', lambda m: '#include "'+str((original.parent/m[1]).resolve())+'"', helper, flags=re.M)
    (destination.parent/original.name).write_text(helper)
