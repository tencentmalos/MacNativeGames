// SPDX-License-Identifier: MIT
#include "guest_tags.h"
#include "profile_c.h"
#include "profiler.h"
#include "port_runtime.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>

extern "C" {
__attribute__((visibility("hidden"))) unsigned mng_guest_tags_active=0;
__attribute__((visibility("hidden"))) uintptr_t mng_guest_tags_originals[32]{};
__attribute__((visibility("hidden"))) void *mng_guest_tags_begin_bridge{},*mng_guest_tags_end_bridge{};
extern char mng_guest_tag_thunks[];
}
namespace {
struct Site {const char* name;uint32_t offset,word;const char* sha16;};
constexpr Site sites[]={
#include "../../games/botw/guest_patches/performance_tags.inc"
};
static_assert(std::size(sites)<=32);
struct Counts {std::atomic<uint64_t> begun{},completed{},abandoned{};};
std::array<Counts,std::size(sites)> counts;
std::atomic<unsigned> installed{};
std::atomic<uint64_t> next_invocation{1},overflows{},mismatches{},epoch{},stale_returns{};
uintptr_t base{},text_end{};
uint32_t(*guest_owner)(){};
std::string error;
struct Span {uint64_t invocation,token;unsigned site;};
thread_local std::array<Span,64> stack;
thread_local unsigned depth{};
thread_local uint64_t retired{};
uint32_t branch(size_t from,size_t to){
    const auto delta=int64_t(to)-int64_t(from);
    if((delta&3) || delta<-(1ll<<27) || delta>=(1ll<<27))throw std::runtime_error("Guest tag branch range");
    return 0x14000000u|(uint32_t(delta/4)&0x03ffffffu);
}
void put32(uint8_t* p,uint32_t word){memcpy(p,&word,4);}
}
extern "C" int mng_guest_tags_requested(){const auto* v=getenv("MNG_GUEST_TAGS");return v && !strcmp(v,"1");}
extern "C" const char* mng_guest_tags_error(){return error.c_str();}
extern "C" int mng_guest_tags_install(uint8_t* image,size_t size,size_t code,size_t allocation,
    const char* build,const char* source,void*(*wrap)(void*,const char*),uint32_t(*owner)()){
    try {
        if(installed || !image || !wrap || !owner)throw std::runtime_error("Guest tag owner/adapter invalid");
        std::string id=build?build:"";
        if((id!="8e9978d50bdd20b4c8395a106c27ffde00000000000000000000000000000000" &&
            id!="8e9978d50bdd20b4c8395a106c27ffde") || !source ||
            strcmp(source,"15cfca7b89348956f85d945fade2e215a6af5991ed1071e181f97ca72f7ae20b"))
            throw std::runtime_error("Guest tag executable identity mismatch");
        if(code%16384 || code<size || code>allocation || allocation-code<16384)
            throw std::runtime_error("Guest tag private code page bounds");
        for(size_t i=0;i<16384;++i)if(image[code+i])throw std::runtime_error("Guest tag code page already owned");
        // All guards and branch encodings are checked before publishing any site.
        for(size_t i=0;i<std::size(sites);++i){const auto &s=sites[i];
            if(s.offset>size || size-s.offset<16 || mng::port::sha256({image+s.offset,16})!=s.sha16)
                throw std::runtime_error(std::string("Guest tag entry mismatch: ")+s.name);
            uint32_t word;memcpy(&word,image+s.offset,4);
            if(word!=s.word)throw std::runtime_error("Guest tag original instruction mismatch");
            branch(s.offset,code+i*32);branch(code+i*32+20,s.offset+4);
        }
        auto* begin=wrap(reinterpret_cast<void*>(mng_guest_tag_begin),"GuestTag.Begin");
        auto* end=wrap(reinterpret_cast<void*>(mng_guest_tag_end),"GuestTag.End");
        if(!begin || !end)throw std::runtime_error("Guest tag host-stack bridge unavailable");
        for(size_t i=0;i<std::size(sites);++i){auto* p=image+code+i*32;const auto &s=sites[i];
            // Near branch -> absolute host assembly thunk. Original first
            // instruction is PC independent, then branches back into Guest.
            put32(p,0x58000050);put32(p+4,0xd61f0200);
            const auto thunk=uintptr_t(mng_guest_tag_thunks+i*16);memcpy(p+8,&thunk,8);
            put32(p+16,s.word);put32(p+20,branch(code+i*32+20,s.offset+4));
            mng_guest_tags_originals[i]=uintptr_t(p+16);
        }
        base=uintptr_t(image);text_end=34773924;guest_owner=owner;
        mng_guest_tags_begin_bridge=begin;mng_guest_tags_end_bridge=end;
        for(size_t i=0;i<std::size(sites);++i)put32(image+sites[i].offset,branch(sites[i].offset,code+i*32));
        installed.store(std::size(sites),std::memory_order_release);error.clear();return 1;
    }catch(const std::exception& e){error=e.what();return 0;}
}
extern "C" void mng_guest_tags_close(){
    __atomic_store_n(&mng_guest_tags_active,0,__ATOMIC_RELEASE);installed=0;base=0;guest_owner=nullptr;
}
extern "C" uint64_t mng_guest_tag_begin(unsigned id,uintptr_t caller,uintptr_t sp,const uint64_t* args){
    if(id>=installed.load(std::memory_order_acquire) || !__atomic_load_n(&mng_guest_tags_active,__ATOMIC_ACQUIRE))return 0;
    if(!spatial::ProfilerRing::Enabled())return 0;
    const auto generation=spatial::ProfilerRing::Generation()+1;
    // A recording transition cannot turn an old open span into a later parent.
    while(depth && stack[depth-1].token!=generation){
        const auto s=stack[--depth];++counts[s.site].abandoned;
        if(s.invocation>retired)retired=s.invocation;
    }
    if(depth==stack.size()){++overflows;return 0;}
    const auto token=mng_profile_begin(sites[id].name);if(!token)return 0;
    const uint64_t invocation=next_invocation.fetch_add(1,std::memory_order_relaxed);
    const auto parent=depth?stack[depth-1].invocation:0;
    stack[depth++]={invocation,token,id};++counts[id].begun;
    mng_profile_counter("Guest.Tag.Id",id);
    mng_profile_counter("Guest.Tag.Invocation",invocation);
    mng_profile_counter("Guest.Tag.Parent",parent);
    mng_profile_counter("Guest.Tag.Epoch",epoch.load(std::memory_order_relaxed));
    mng_profile_counter("Guest.Tag.Owner",guest_owner());
    mng_profile_counter("Guest.Tag.Module",0);
    mng_profile_counter("Guest.Tag.Offset",sites[id].offset);
    mng_profile_counter("Guest.Tag.CallerModule",caller>=base && caller-base<text_end?0:-1);
    mng_profile_counter("Guest.Tag.CallerOffset",caller>=base && caller-base<text_end?caller-base:-1);
    mng_profile_counter("Guest.Tag.Stack",sp);
    mng_profile_counter("Guest.Tag.Object",args[0]);
    if(id==7){ // WorkerMgr::pushJobQueue; proven register-only ABI.
        mng_profile_counter("Guest.Job.Queue",args[2]);
        mng_profile_counter("Guest.Job.NameOffset",args[1]>=base && args[1]-base<47644672?args[1]-base:-1);
        mng_profile_counter("Guest.Job.CoreMask",args[3]);
        mng_profile_counter("Guest.Job.SyncType",args[4]);
    }
    return invocation;
}
extern "C" void mng_guest_tag_end(uint64_t invocation){
    if(!invocation)return;
    if(invocation<=retired){++stale_returns;return;}
    if(!depth || stack[depth-1].invocation!=invocation){++mismatches;return;}
    const auto s=stack[--depth];
    if(s.token==spatial::ProfilerRing::Generation()+1){mng_profile_end(s.token);++counts[s.site].completed;}
    else ++counts[s.site].abandoned;
}
std::string mng::profile::guestTagsCommand(const std::string& action){
    if(action!="status" && action!="start" && action!="stop")return "error=invalid_tags_action\n";
    if(action=="start" && !installed.load(std::memory_order_acquire))return "error=guest_tags_not_installed\nstartup=MNG_GUEST_TAGS=1\n";
    if(action!="status"){
        const unsigned enabled=action=="start";
        if(__atomic_exchange_n(&mng_guest_tags_active,enabled,__ATOMIC_ACQ_REL)!=enabled)++epoch;
    }
    std::ostringstream out;out<<"schema=mng.guest-tags.v1\ntags="<<__atomic_load_n(&mng_guest_tags_active,__ATOMIC_ACQUIRE)
        <<"\ninstalled="<<installed.load()<<"\nepoch="<<epoch.load()<<"\noverflow="<<overflows.load()<<"\nmismatch="<<mismatches.load()
        <<"\nstale_returns="<<stale_returns.load()
        <<"\nsource=exact_botw160_native_entry_forwarding\nframe_identity=phase_invocation_not_present\n";
    for(size_t i=0;i<std::size(sites);++i)out<<"tag_"<<i<<"="<<sites[i].name<<"\ntag_"<<i<<"_begun="<<counts[i].begun.load()
        <<"\ntag_"<<i<<"_completed="<<counts[i].completed.load()<<"\ntag_"<<i<<"_abandoned="<<counts[i].abandoned.load()<<'\n';
    return out.str();
}
