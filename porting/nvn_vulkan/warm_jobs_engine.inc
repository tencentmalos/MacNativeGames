// SPDX-License-Identifier: MIT
// Included in warm. Only immutable Rec inputs cross to Foundation WORK_JOB.
std::unique_ptr<mng::jobs::KeyedJobs> job_queue;
unsigned job_workers=0;
bool async_on=false;
std::atomic<uint64_t> async_built{0},async_skips{0};
std::string record_bytes(const Rec& rec){return std::string(reinterpret_cast<const char*>(rec.words.data()),rec.words.size()*4);}
bool enqueue(Rec rec,const char* prefix=""){
    if(!job_queue)return false;
    auto key=std::string(prefix)+record_bytes(rec);size_t bytes=rec.words.size()*4;
    for(const auto& k:rec.keys)bytes+=k.size();
    auto input=std::make_shared<const Rec>(std::move(rec));
    return job_queue->enqueue(std::move(key),bytes,[input]{
        uint64_t t0=timing_clock();bool ok=build(*input);
        if(ok)++built;else ++failed;
        build_ns+=timing_clock()-t0;return ok;
    });
}
mng::jobs::KeyedJobs::Stats job_snapshot(){return job_queue?job_queue->stats():mng::jobs::KeyedJobs::Stats{};}
void job_stats(){
    auto s=job_snapshot();
    xbg_logf("NVN PSO_JOBS workers=%u submitted=%llu built=%llu failed=%llu active=%llu peak=%llu joined=%llu claimed=%llu dedup=%llu rejected=%llu retained_bytes=%llu",
        job_workers,(unsigned long long)s.submitted,(unsigned long long)s.built,(unsigned long long)s.failed,
        (unsigned long long)s.active,(unsigned long long)s.peak,(unsigned long long)s.waits,
        (unsigned long long)s.claimed,(unsigned long long)s.deduplicated,(unsigned long long)s.rejected,(unsigned long long)s.bytes);
}
void job_counters(){
    const auto s=job_snapshot();
    spatial::LiteTrace::counter("NVN.PSO.Workers",job_workers);
    spatial::LiteTrace::counter("NVN.PSO.Submitted",s.submitted);
    spatial::LiteTrace::counter("NVN.PSO.Built",s.built);
    spatial::LiteTrace::counter("NVN.PSO.Failed",s.failed);
    spatial::LiteTrace::counter("NVN.PSO.Active",s.active);
    spatial::LiteTrace::counter("NVN.PSO.Peak",s.peak);
    spatial::LiteTrace::counter("NVN.PSO.Joined",s.waits);
    spatial::LiteTrace::counter("NVN.PSO.Claimed",s.claimed);
}
