# SPDX-License-Identifier: MIT
# Adapt the pinned, read-only D2R engine to the shared Foundation work queue.
def adapt(text, adapter):
    def replace(old, new):
        nonlocal text
        if text.count(old) != 1:
            raise SystemExit('Pinned PSO warmup patch mismatch: '+old[:100])
        text=text.replace(old,new)
    text='#include "'+str(adapter/'../jobs/keyed_jobs.h')+'"\n'+text
    replace('void flush();void register_module', 'void job_stats();void job_counters();void flush();void register_module')
    replace('std::vector<std::thread> threads;std::atomic<bool> stop{false};std::atomic<size_t> next{0};','std::atomic<bool> stop{false};')
    replace('std::lock_guard<std::mutex> l(mu);set_layouts.push_back(sl);layouts.emplace(k,pl);return pl;',
        'std::lock_guard<std::mutex> l(mu);auto ins=layouts.emplace(k,pl);if(!ins.second){vkDestroyPipelineLayout(cfg.device,pl,nullptr);vkDestroyDescriptorSetLayout(cfg.device,sl,nullptr);return ins.first->second;}set_layouts.push_back(sl);return pl;')
    start=text.index('void worker(){\n    setpriority(PRIO_PROCESS,0,5);')
    end=text.index('// 首次启动还没有列表。',start)
    text=text[:start]+'#include "'+str(adapter/'warm_jobs_engine.inc')+'"\n'+text[end:]
    replace('if(!enabled || recs.empty())return;stop=false;next=0;unsigned n=std::min<unsigned>(3,std::max(1u,std::thread::hardware_concurrency()/3));\n    for(unsigned i=0;i<n;i++)threads.emplace_back(worker);',
        '''if(!enabled)return;stop=false;
    const char* setting=getenv("MNG_PSO_WORKERS");unsigned requested=setting?unsigned(atoi(setting)):0;
    job_workers=requested?std::clamp(requested,1u,8u):std::min(4u,std::max(1u,std::thread::hardware_concurrency()/2));
    job_queue=std::make_unique<mng::jobs::KeyedJobs>(job_workers,pipeline_object_limit,64u<<20);
    for(auto& rec:recs)enqueue(std::move(rec));recs.clear();
    job_stats();''')
    # Bound both the parsed record set and the retained pipeline set. Additional
    # keys follow the normal foreground creation path; never omit their draw.
    replace('recs.push_back(std::move(r));}}','recs.push_back(std::move(r));if(recs.size()>=pipeline_object_limit)break;}}')
    start=text.index('void shutdown(){{std::lock_guard<std::mutex> l(mu);stop=true;}async_cv')
    end=text.index('    for(auto &p:ready)',start)
    text=text[:start]+'''void shutdown(){stop=true;if(job_queue){job_queue->stop();job_stats();job_queue.reset();}std::lock_guard<std::mutex> l(mu);
'''+text[end:]
    replace('bool take(const std::string &bytes,VkPipeline &out){std::lock_guard<std::mutex> l(mu);',
        'bool take(const std::string &bytes,VkPipeline &out){if(job_queue)job_queue->acquire(bytes);std::lock_guard<std::mutex> l(mu);')
    replace('    // A draw that may be skipped gets no pipeline while its build is queued (warm::async_request); it is not cached.\n    else if(graphics && cacheable && may_skip && warm::async_request(*graphics,key,bytes)){out.pipeline=VK_NULL_HANDLE;out.transient=true;++warm::async_skips;return;}\n','')
    # This port never delays visibility by dropping draws. Known preparation is
    # acquired at the ordered consumer; unknown keys use the synchronous path.
    replace('xbg_logf("NVN background builds for skippable draws=%d (marker gfx-async-pipelines)",int(warm::async_on));',
        'warm::async_on=false;xbg_logf("NVN PSO ordered publication enabled; draw skipping disabled");')
    replace('warm::hits=warm::creates=warm::create_ns=warm::create_max_ns=0;',
        'warm::job_stats();warm::hits=warm::creates=warm::create_ns=warm::create_max_ns=0;')
    replace('spatial::LiteTrace::counter("NVN.Translate.ElapsedNs",elapsed);',
        'warm::job_counters();spatial::LiteTrace::counter("NVN.Translate.ElapsedNs",elapsed);')
    text+=r'''
extern "C" int mng_warm_pipeline_probe(char* out,unsigned bytes){
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if(!warm::job_queue){snprintf(out,bytes,"warm queue disabled");return -1;}
    // Reconstruct owned input from actual graphics pipeline keys produced by
    // the test draws; never pass a live VkCreateInfo's pointers to a worker.
    std::vector<warm::Rec> selected;
    for(const auto& [key,entry]:pipeline_objects){
        warm::Rec rec;rec.words.resize(key.size()/4);memcpy(rec.words.data(),key.data(),key.size());
        warm::Reader r{rec.words.data(),rec.words.size()};if(r.u()!=1)continue;
        uint32_t n=r.u();if(r.i+n>r.n)continue;r.i+=n;
        n=r.u();if(r.i+n>r.n)continue;r.i+=n;
        r.u();r.u();uint32_t ns=r.u();if(!ns || ns>5)continue;bool good=true;
        {std::lock_guard<std::mutex> lock(warm::mu);
        for(uint32_t i=0;i<ns;i++){r.u();r.u();auto id=r.q();auto f=warm::id_keys.find(id);if(f==warm::id_keys.end()){good=false;break;}rec.keys.push_back(f->second);}}
        if(good && !r.bad)selected.push_back(std::move(rec));
        if(selected.size()==4)break;
    }
    if(selected.size()!=4){snprintf(out,bytes,"cannot recover four graphics records");return -2;}
    auto before=warm::job_snapshot();
    for(const auto& r:selected){if(!warm::enqueue(r,"probe:") || !warm::enqueue(r,"probe:"))return -3;}
    for(const auto& r:selected){if(!warm::job_queue->acquire("probe:"+warm::record_bytes(r)))return -6;VkPipeline result{};if(!warm::take(warm::record_bytes(r),result))return -4;vkDestroyPipeline(cfg.device,result,nullptr);}
    auto after=warm::job_snapshot();
    snprintf(out,bytes,"PSO jobs workers=%u built=%llu dedup=%llu peak=%llu joined=%llu claimed=%llu",
        warm::job_workers,(unsigned long long)(after.built-before.built),(unsigned long long)(after.deduplicated-before.deduplicated),
        (unsigned long long)after.peak,(unsigned long long)after.waits,(unsigned long long)after.claimed);
    return after.built-before.built==4 && after.deduplicated-before.deduplicated==4?0:-5;
}
'''
    return text
