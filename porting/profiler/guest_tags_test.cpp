// SPDX-License-Identifier: MIT
// Optional private rewritten image is only read to validate guarded installation.
// No game instruction is executed: all forwarded bodies below are test functions.
#include "guest_tags.h"
#include "profiler.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>
extern "C" {
extern char mng_guest_tag_thunks[];
extern uintptr_t mng_guest_tags_originals[32];
}
using U=uint64_t;
using F=U(*)(U,U,U,U,U,U,U,U);
static F entry(unsigned i){return reinterpret_cast<F>(mng_guest_tag_thunks+16*i);}
static U integer(U a,U b,U c,U d,U e,U f,U g,U h){return a+2*b+3*c+4*d+5*e+6*f+7*g+8*h;}
static U nested(U a,U b,U c,U d,U e,U f,U g,U h){return entry(1)(a,b,c,d,e,f,g,h)+17;}
static double floating(double a,double b,double c,double d,double e,double f,double g,double h){return a+2*b+3*c+4*d+5*e+6*f+7*g+8*h;}
struct Aggregate { U a,b,c,d; };
static Aggregate indirect(U a,U b){return {a,b,a+b,a-b};}
static void* wrap(void* fn,const char*){return fn;}
static uint32_t owner(){return 77;}
int main(int argc,char** argv){
    assert(argc==2);
    std::ifstream input(argv[1],std::ios::binary|std::ios::ate);assert(input);
    const size_t size=input.tellg(),code=(size+16383)&~size_t(16383);
    std::vector<uint8_t> image(code+16384);input.seekg(0);input.read((char*)image.data(),size);assert(input);
    const auto original=image;
    const char* build="8e9978d50bdd20b4c8395a106c27ffde00000000000000000000000000000000";
    const char* sha="15cfca7b89348956f85d945fade2e215a6af5991ed1071e181f97ca72f7ae20b";
    auto install=[&](const char* b,const char* s,size_t at){return mng_guest_tags_install(image.data(),size,at,image.size(),b,s,wrap,owner);};
    assert(!install("wrong",sha,code));assert(image==original);
    assert(!install(build,"wrong",code));assert(image==original);
    assert(!install(build,sha,code-4));assert(image==original);
    image[0x150a640]^=1;const auto corrupted=image;
    assert(!install(build,sha,code));assert(image==corrupted);image=original;
    assert(install(build,sha,code));assert(!install(build,sha,code));
    const auto directory=std::filesystem::temp_directory_path()/"mng-guest-tags-probe";
    mng::profile::initialize("mng-guest-tags-probe",directory.string(),"synthetic-forwarding;private-entry-guards-only");
    for(unsigned i=0;i<12;++i)mng_guest_tags_originals[i]=(uintptr_t)integer;
    for(unsigned i=0;i<12;++i)assert(entry(i)(1,2,3,4,5,6,7,8)==204);
    assert(mng::profile::guestTagsCommand("status").find("tag_0_begun=0\n")!=std::string::npos);
    mng::profile::command("profiler_ring start");mng::profile::guestTagsCommand("start");
    for(unsigned i=0;i<12;++i)assert(entry(i)(1,2,3,4,5,6,7,8)==204);
    mng_guest_tags_originals[0]=(uintptr_t)nested;assert(entry(0)(1,2,3,4,5,6,7,8)==221);
    mng_guest_tags_originals[0]=(uintptr_t)floating;
    using FP=double(*)(double,double,double,double,double,double,double,double);
    assert(reinterpret_cast<FP>(entry(0))(1,2,3,4,5,6,7,8)==204);
    mng_guest_tags_originals[0]=(uintptr_t)indirect;
    auto result=reinterpret_cast<Aggregate(*)(U,U)>(entry(0))(71,13);
    assert(result.a==71 && result.b==13 && result.c==84 && result.d==58);
    uint64_t args[8]{};
    auto old=mng_guest_tag_begin(0,0,0,args);
    mng::profile::command("profiler_ring stop");mng::profile::command("profiler_ring start");
    auto fresh=mng_guest_tag_begin(1,0,0,args);mng_guest_tag_end(fresh);mng_guest_tag_end(old);
    assert(mng::profile::guestTagsCommand("status").find("mismatch=0\n")!=std::string::npos);
    mng::profile::guestTagsCommand("stop");mng::profile::command("profiler_ring stop");mng_guest_tags_close();
    puts("PASS: identity/entry/page guards are atomic; all 12 thunks, nested scopes, x0-x7, FP arguments/return, x8 indirect result and recording transitions");
}
