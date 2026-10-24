// SPDX-License-Identifier: MIT
#include "port_runtime.h"
#include "../../android/third_party/d2r/sha256.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <memory>
#include <set>
#include <stdexcept>

extern "C" {
__attribute__((visibility("default"),used)) MngPortDebugDescriptor mng_port_debug_descriptor={1,sizeof(MngPortDebugDescriptor),0,0,0,0};
__attribute__((visibility("default"),noinline,used)) void mng_port_debug_checkpoint() {
    // Stable function breakpoint, never an unsolicited SIGTRAP.
    std::atomic_signal_fence(std::memory_order_seq_cst);
}
}
namespace mng::port {
namespace {
void check(bool value,const char* reason) {if(!value)throw std::runtime_error(reason);}
bool hex(const std::string& s,size_t length) {
    return s.size()==length && std::all_of(s.begin(),s.end(),[](unsigned char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
std::string build_id(std::string s) {
    check(!s.empty() && s.size()<=64 && s.size()%2==0 && hex(s,s.size()),"Invalid lowercase Build ID");
    while(s.size()>2 && s.ends_with("00"))s.resize(s.size()-2);
    check(s!="00","Empty Build ID");return s;
}
void validate(const Identity& i) {
    check(!i.profile.empty() && !i.module.empty(),"Missing profile/module identity");
    check(i.platform=="android-arm64" || i.platform=="macos-arm64","Unsupported host ABI");
    build_id(i.build_id);
    check(hex(i.source_sha256,64)&&hex(i.image_sha256,64),"Invalid source/rewrite SHA-256");
}
bool same(const Identity& a,const Identity& b) {
    validate(a);validate(b);
    return a.profile==b.profile && a.platform==b.platform && a.module==b.module &&
        build_id(a.build_id)==build_id(b.build_id) && a.source_sha256==b.source_sha256 && a.image_sha256==b.image_sha256;
}
std::mutex debug_mutex;
uint64_t next_generation{};
std::string debug_payload;
void begin_update() {++mng_port_debug_descriptor.sequence;std::atomic_thread_fence(std::memory_order_seq_cst);}
void end_update() {std::atomic_thread_fence(std::memory_order_seq_cst);++mng_port_debug_descriptor.sequence;}
}
std::string sha256(std::span<const uint8_t> bytes) {
    mng_port_sha256_CTX ctx;mng_port_sha256_Init(&ctx);
    while(!bytes.empty()) {
        const auto n=std::min<size_t>(bytes.size(),UINT32_MAX);
        mng_port_sha256_Update(&ctx,bytes.data(),mng_port_sha_size(n));bytes=bytes.subspan(n);
    }
    unsigned char digest[32];mng_port_sha256_Final(digest,&ctx);char result[65];
    for(unsigned i=0;i<32;++i)snprintf(result+i*2,3,"%02x",digest[i]);return result;
}
PatchReceipt apply_staged(std::span<uint8_t> image,const Identity& identity,
    std::span<const Patch> patches,std::span<const std::string> selected,bool allow_features,bool allow_diagnostics) {
    validate(identity);
    check(sha256(image)==identity.image_sha256,"Staged image digest mismatch");
    std::set<std::string> ids,requested(selected.begin(),selected.end());
    check(requested.size()==selected.size(),"Duplicate selected patch ID");
    std::vector<const Patch*> active;
    for(const auto& p:patches) {
        check(!p.id.empty()&&ids.insert(p.id).second,"Duplicate/empty patch ID");
        if(!requested.contains(p.id))continue;
        check(same(identity,p.identity),"Patch executable or platform identity mismatch");
        check(!p.reason.empty()&&!p.evidence.empty(),"Patch reason/evidence missing");
        check(p.kind==PatchKind::Compatibility || (p.kind==PatchKind::Feature&&allow_features) ||
              (p.kind==PatchKind::Diagnostic&&allow_diagnostics),"Patch category not enabled");
        check(!p.expected.empty() && p.expected.size()==p.replacement.size(),"Patch size mismatch");
        check(p.offset%4==0 && p.expected.size()%4==0,"Instruction patch alignment mismatch");
        check(p.offset<=image.size()&&p.expected.size()<=image.size()-p.offset,"Patch outside staged image");
        check(std::equal(p.expected.begin(),p.expected.end(),image.begin()+p.offset),"Patch original-byte mismatch");
        active.push_back(&p);
    }
    check(active.size()==requested.size(),"Unknown selected patch ID");
    std::sort(active.begin(),active.end(),[](auto* a,auto* b){return a->offset<b->offset;});
    for(size_t i=1;i<active.size();++i)check(active[i-1]->offset+active[i-1]->expected.size()<=active[i]->offset,"Selected patches overlap");
    // Allocate the result and calculate its digest before touching the input.
    // Allocation/digest failures therefore cannot leave a partially patched image.
    auto result=std::vector<uint8_t>(image.begin(),image.end());
    PatchReceipt receipt{identity,identity.image_sha256,{}, {}};
    for(const auto* p:active) {
        std::copy(p->replacement.begin(),p->replacement.end(),result.begin()+p->offset);
        receipt.applied.push_back({p->id,p->kind,p->offset,p->expected.size(),sha256(p->expected),
                                  sha256(p->replacement),p->reason,p->evidence});
    }
    receipt.output_sha256=sha256(result);
    std::copy(result.begin(),result.end(),image.begin());return receipt;
}
DebugSession::DebugSession(bool enabled) {
    if(!enabled)return;
    std::lock_guard lock(debug_mutex);
    check(!mng_port_debug_descriptor.generation,"A port debug session is already active");
    check(next_generation<std::numeric_limits<uint64_t>::max(),"Debug generation exhausted");
    generation_=++next_generation;
    begin_update();mng_port_debug_descriptor.generation=generation_;end_update();
}
void DebugSession::publish(std::string payload) {
    if(!generation_)return;
    check(!payload.empty()&&payload.size()<=(8u<<20),"Debug payload outside limit");
    {
        std::lock_guard lock(debug_mutex);
        check(mng_port_debug_descriptor.generation==generation_,"Stale port debug owner");
        begin_update();debug_payload.swap(payload);
        mng_port_debug_descriptor.payload_address=reinterpret_cast<uintptr_t>(debug_payload.data());
        mng_port_debug_descriptor.payload_size=debug_payload.size();end_update();
        const char* log=getenv("MNG_PORT_DEBUG_LOG");
        if(log&&std::strcmp(log,"1")==0) {
            // Optional private stage log, never used on a signal/realtime path.
            fwrite(debug_payload.data(),1,debug_payload.size(),stdout);fputc('\n',stdout);fflush(stdout);
        }
    }
    mng_port_debug_checkpoint();
}
DebugSession::~DebugSession() {
    if(!generation_)return;
    std::lock_guard lock(debug_mutex);
    if(mng_port_debug_descriptor.generation!=generation_)return;
    begin_update();mng_port_debug_descriptor.payload_address=0;mng_port_debug_descriptor.payload_size=0;
    mng_port_debug_descriptor.generation=0;debug_payload.clear();end_update();
}
}

namespace {
std::mutex c_owner_mutex;
std::unique_ptr<mng::port::DebugSession> c_owner;
}
extern "C" uint64_t mng_port_debug_open() {
    try {std::lock_guard lock(c_owner_mutex);if(c_owner)return 0;
        c_owner=std::make_unique<mng::port::DebugSession>(true);return c_owner->generation();}
    catch(...){return 0;}
}
extern "C" int mng_port_debug_publish(uint64_t generation,const char* payload,size_t length) {
    try {std::lock_guard lock(c_owner_mutex);
        if(!c_owner||c_owner->generation()!=generation||!payload||!length||length>(8u<<20))return 0;
        c_owner->publish(std::string(payload,length));return 1;}
    catch(...){return 0;}
}
extern "C" void mng_port_debug_close(uint64_t generation) {
    std::lock_guard lock(c_owner_mutex);if(c_owner&&c_owner->generation()==generation)c_owner.reset();
}
