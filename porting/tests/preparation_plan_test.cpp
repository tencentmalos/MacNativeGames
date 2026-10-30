// SPDX-License-Identifier: MIT
// Compile the production host-only plans/key cache against a synthetic renderer.
// No Vulkan runtime, Guest executable, or device is required.
#include "../nvn_vulkan/preparation_plan.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <random>
#include <set>
#include <stdexcept>
using namespace mng::vulkan::preparation;
enum { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER=1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER=4,
       VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER=5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER=6,
       VK_DESCRIPTOR_TYPE_STORAGE_BUFFER=7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE=3 };
enum { XBG_SOURCE_UNIFORM=1, XBG_SOURCE_STORAGE, XBG_SOURCE_SUPPORT, XBG_SOURCE_C1,
       XBG_SOURCE_DRIVER0, XBG_SOURCE_DRIVER2, XBG_SOURCE_TEXTURE, XBG_SOURCE_IMAGE };
struct xbg_shader_binding { uint32_t binding,type,count,bytes,source,slot,sampler_slot,flags; };
struct Shader {
    std::vector<xbg_shader_binding> bindings;
    std::set<uint32_t> storage_writes;
    ShaderPlan<xbg_shader_binding> preparation;
};
struct Range { uint64_t address{},size{}; };
struct {
    Shader *shader[6]{};
    Range ub[6][16]{},sb[6][16]{};
    uint64_t tex[6][128]{},samp[6][128]{},img[6][16]{};
    struct { float width{64},height{32}; } viewport;
    struct { unsigned alpha_func{8}; } color;
} state;
constexpr uint32_t nvn_stage[]{0,3,4,2,1,5};
uint64_t tex_gen[6]{},buf_gen[6]{};
struct { bool active{}; } capture;
FILE *provenance_file{};
double g_ui_moff{};
struct { struct { uint64_t maxStorageBufferRange{1u<<20}; } limits; } properties;
namespace mng_translate {
bool on=true,descriptors=true;
bool &preparation_plan(){return on;}
bool descriptor_plan(){return descriptors;}
}
namespace spatial { struct LiteTrace { static void counter(const char*,uint64_t){} }; }
void need(bool value,const char *message){if(!value)throw std::runtime_error(message);}
#include "../nvn_vulkan/preparation_plan_engine.inc"
struct DynUbo { uint64_t order,address;size_t copy; };
std::atomic<uint64_t> memo_epoch{1};
uint64_t desc_memo_why[6]{};
bool texel_memo_enabled=true;
#define UBO_DYN(mode,b) ((b).type==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && ((mode)==1 || ((mode)==2 && ((b).source==XBG_SOURCE_UNIFORM||(b).source==XBG_SOURCE_DRIVER0||(b).source==XBG_SOURCE_DRIVER2))))
bool mng_descriptor_key_legacy(std::vector<uint64_t> &key,std::vector<uint8_t> &tags,bool tagged,unsigned mode,std::vector<DynUbo> &dynamic){
    auto word=[&](uint64_t value,uint8_t tag){key.push_back(value);if(tagged)tags.push_back(tag);};
    for(unsigned st=0;st<5;++st)if(auto *shader=state.shader[st])for(const auto &b:shader->bindings){
        if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER || (b.type==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER && !texel_memo_enabled))return false;
        if(b.type==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || (b.type==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && shader->storage_writes.count(b.binding)))return false;
        for(unsigned element=0;element<b.count;++element){const auto ns=nvn_stage[st],slot=b.slot+element;
            switch(b.source){
            case XBG_SOURCE_UNIFORM:case XBG_SOURCE_STORAGE:{
                if(slot>=16)return false;
                const auto r=b.source==XBG_SOURCE_UNIFORM?state.ub[ns][slot]:state.sb[ns][slot];
                if(UBO_DYN(mode,b) && b.source==XBG_SOURCE_UNIFORM && r.address && r.size){
                    const size_t copy=std::min<size_t>(r.size,b.bytes?b.bytes:65536);word(copy,2);
                    if(!mng_translate::descriptor_plan())dynamic.push_back({(uint64_t(b.binding)<<32)|element,r.address,copy});
                }else{word(r.address,b.source==XBG_SOURCE_UNIFORM?3:10);word(r.size,4);}
                break;}
            case XBG_SOURCE_SUPPORT:word((uint64_t(uint32_t(state.viewport.width))<<32)|uint32_t(state.viewport.height),5);word(state.color.alpha_func,6);break;
            case XBG_SOURCE_C1:break;
            case XBG_SOURCE_DRIVER0:case XBG_SOURCE_DRIVER2:word(UBO_DYN(mode,b)?uint64_t(0xD0D0+b.source):(b.source==XBG_SOURCE_DRIVER0?tex_gen[ns]:buf_gen[ns]),7);break;
            case XBG_SOURCE_TEXTURE:case XBG_SOURCE_IMAGE:
                if(slot>=(b.source==XBG_SOURCE_IMAGE?16u:128u))return false;
                word(b.source==XBG_SOURCE_IMAGE?state.img[ns][slot]:state.tex[ns][slot],8);
                if(b.sampler_slot!=UINT32_MAX){if(b.sampler_slot>=128)return false;word(state.samp[ns][b.sampler_slot],9);}break;
            default:return false;
            }
        }
    }
    return true;
}
#include "../nvn_vulkan/preparation_key_engine.inc"

void revisions_test(){
    uint64_t revision=1;std::array<uint32_t,4> values{1,2,3,4},same=values;
    assert(!assign(values,same,{&revision}) && revision==1);
    same[2]=9;assert(assign_bytes(values.data()+2,same.data()+2,4,{&revision}) && revision==2);
    assert(values[0]==1 && values[1]==2 && values[2]==9 && values[3]==4);
    BufferRevision version;Range range{0x1000,16};
    assign_range(range,Range{0x2000,16},version);assert(version.address==2 && version.shape==1);
    assign_range(range,range,version);assert(version.address==2 && version.shape==1);
    assign_range(range,Range{0,16},version);assert(version.shape==2);
    assign_range(range,Range{0x2000,0},version);const auto previous=version.shape;
    assign_range(range,Range{0x3000,0},version);assert(version.shape==previous+1);
    RevisionWatch watch;watch.add(version.address);watch.add(version.address);watch.add(version.shape);
    assert(watch.sources.size()==2 && !watch.unchanged());watch.remember();assert(watch.unchanged());
    ++version.shape;assert(!watch.unchanged());
}
void metadata_test(){
    ShaderPlan<xbg_shader_binding> plan;
    std::vector<xbg_shader_binding> bindings{{9,6,2,16,1,3,UINT32_MAX,0},{2,7,1,0,2,1,UINT32_MAX,0}};
    auto make=[&]{return build(plan,bindings,[](auto b){return b.type==7;},[](auto b){return b.type==6;},[](auto b){return b.source==1;});};
    assert(make() && plan.uniforms==2 && plan.varying_uniforms==2 && plan.elements.size()==3);
    assert(plan.elements[0].group==0 && plan.elements[0].element==0 && plan.elements[1].element==1 && plan.elements[2].group==1);
    auto copy=plan;bindings[0].slot=8;assert(plan.groups[0].binding.slot==3 && copy.groups[0].binding.slot==3);
    bindings[0].count=0;assert(!make() && plan.ready && !plan.valid);
    bindings[0].count=129;assert(!make());bindings.assign(33,{0,6,128,16,1,0,UINT32_MAX,0});assert(!make());
    std::vector<StorageRange> ranges{{64,96,false},{16,48,false},{32,80,true},{96,100,false}};
    merge_ranges(ranges);assert(ranges.size()==2 && ranges[0].begin==16 && ranges[0].end==96 && ranges[0].written && ranges[1].begin==96);
}
void compare_key(unsigned mode){
    std::vector<uint64_t> actual,expected;std::vector<uint8_t> tags,expected_tags;std::vector<DynUbo> a,b;
    const bool ok=mng_descriptor_key(actual,tags,true,mode,a);
    const bool reference=mng_descriptor_key_legacy(expected,expected_tags,true,mode,b);
    assert(ok==reference && actual==expected && tags==expected_tags && a.size()==b.size());
    for(size_t i=0;i<a.size();++i)assert(a[i].order==b[i].order && a[i].address==b[i].address && a[i].copy==b[i].copy);
}
void randomized_keys_test(){
    std::mt19937 random(0x264e564e);Shader shaders[3];
    shaders[0].bindings={{31,6,2,32,XBG_SOURCE_UNIFORM,2,UINT32_MAX,0},{1,1,2,0,XBG_SOURCE_TEXTURE,4,3,0},
        {7,6,1,0,XBG_SOURCE_SUPPORT,0,UINT32_MAX,0},{13,6,1,0,XBG_SOURCE_DRIVER0,0,UINT32_MAX,0},
        {19,6,1,0,XBG_SOURCE_DRIVER2,0,UINT32_MAX,0},{25,6,1,0,XBG_SOURCE_C1,0,UINT32_MAX,0}};
    shaders[1].bindings={{4,7,2,0,XBG_SOURCE_STORAGE,6,UINT32_MAX,0},{10,1,1,0,XBG_SOURCE_IMAGE,1,UINT32_MAX,0},
        {16,6,1,0,XBG_SOURCE_STORAGE,8,UINT32_MAX,0}};
    shaders[2]=shaders[0];shaders[2].preparation={};shaders[2].bindings.push_back({37,6,1,16,XBG_SOURCE_UNIFORM,7,UINT32_MAX,0});
    state.shader[0]=&shaders[0];state.shader[4]=&shaders[1];
    for(unsigned ns=0;ns<6;++ns)for(unsigned slot=0;slot<16;++slot){state.ub[ns][slot]={0x1000+slot*256,32};state.sb[ns][slot]={0x4000+slot*256,256};}
    for(unsigned step=0;step<30000;++step){
        const unsigned ns=random()%6,slot=random()%16,kind=random()%12;
        if(kind<3){auto &range=kind==2?state.sb[ns][slot]:state.ub[ns][slot];auto &revision=kind==2?mng_prepare::versions.storage[ns][slot]:mng_prepare::versions.uniform[ns][slot];
            Range next{uint64_t(random()%5)*256,uint64_t(random()%4)*16};assign_range(range,next,revision);++buf_gen[ns];}
        else if(kind<6){const auto index=random()%16;uint64_t value=random()%32;
            auto &target=kind==3?state.tex[ns][index]:kind==4?state.samp[ns][index]:state.img[ns][index];
            auto &revision=kind==3?mng_prepare::versions.textures[ns]:kind==4?mng_prepare::versions.samplers[ns]:mng_prepare::versions.images[ns];
            assign(target,value,{&revision});++tex_gen[ns];}
        else if(kind==6){float width=float(random()%256+1);assign(state.viewport.width,width,{&mng_prepare::versions.support});}
        else if(kind==7){unsigned alpha=random()%9;assign(state.color.alpha_func,alpha,{&mng_prepare::versions.support});}
        else if(kind==8)state.shader[0]=&shaders[(random()&1)?0:2];
        else if(kind==9)++memo_epoch;
        else if(kind==10)mng_translate::descriptors=!(random()%5==0);
        else{mng_translate::on=!(random()%5==0);mng_prepare::invalidate();}
        for(unsigned mode=0;mode<3;++mode){compare_key(mode);compare_key(mode);}
    }
    assert(mng_prepare::counts.key_builds && mng_prepare::counts.key_reuses && mng_prepare::counts.fallbacks);
    // Content changes do not alter descriptor identity; consumers still snapshot.
    mng_translate::on=mng_translate::descriptors=true;state.shader[0]=&shaders[0];
    compare_key(1);auto reuse=mng_prepare::counts.key_reuses;compare_key(1);assert(mng_prepare::counts.key_reuses>reuse);
    mng_prepare::reset();state.shader[0]=state.shader[4]=nullptr;
}
void ranges_and_clone_test(){
    mng_translate::on=true;Shader shader;
    shader.bindings={{0,7,2,0,XBG_SOURCE_STORAGE,0,UINT32_MAX,0}};shader.storage_writes.insert(0);
    state.shader[5]=&shader;state.sb[5][0]={0x1000,128};state.sb[5][1]={0x1040,128};
    auto *ranges=mng_prepare::storage_ranges(true);assert(ranges && ranges->size()==1 && ranges->front().written && ranges->front().end==0x10c0);
    const auto built=mng_prepare::counts.range_builds;ranges=mng_prepare::storage_ranges(true);assert(ranges && mng_prepare::counts.range_builds==built);
    assign_range(state.sb[5][1],Range{0x2000,128},mng_prepare::versions.storage[5][1]);
    ranges=mng_prepare::storage_ranges(true);assert(ranges && ranges->size()==2 && mng_prepare::counts.range_builds==built+1);
    auto clone=shader;clone.preparation={};clone.bindings.push_back({6,6,1,16,XBG_SOURCE_UNIFORM,2,UINT32_MAX,0});
    state.shader[5]=&clone;auto *plan=mng_prepare::bindings(true);assert(plan && plan->elements.size()==3 && plan->uniforms==1);
    assert(shader.preparation.elements.size()==2);
    capture.active=true;assert(!mng_prepare::bindings(true));capture.active=false;
    mng_prepare::reset();state.shader[5]=nullptr;
}
void fallback_bounds_test(){
    mng_translate::on=mng_translate::descriptors=true;
    Shader shader;shader.bindings={{0,6,1,16,XBG_SOURCE_UNIFORM,0,UINT32_MAX,0}};
    state.shader[0]=&shader;mng_prepare::metadata_bytes=mng_prepare::metadata_budget;
    assert(!mng_prepare::bindings(false) && shader.preparation.ready && !shader.preparation.valid);
    assert(mng_prepare::counts.metadata_fallbacks);compare_key(1);
    mng_prepare::reset();state.shader[0]=nullptr;
    Shader texel;texel.bindings={{0,VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,1,0,XBG_SOURCE_TEXTURE,0,UINT32_MAX,0}};
    state.shader[0]=&texel;texel_memo_enabled=true;compare_key(1);compare_key(1);
    texel_memo_enabled=false;compare_key(1);texel_memo_enabled=true;compare_key(1);
    assert(mng_prepare::counts.key_ineligible);
    mng_prepare::reset();state.shader[0]=nullptr;
}
int main(){
    setenv("MNG_PREPARATION_VALIDATE","1",1);
    revisions_test();metadata_test();randomized_keys_test();ranges_and_clone_test();fallback_bounds_test();
    puts("Preparation revisions, immutable metadata, range reuse and 180000 legacy-key comparisons passed");
}
