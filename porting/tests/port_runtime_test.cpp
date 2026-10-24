// SPDX-License-Identifier: MIT
#include "port_runtime.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <chrono>
using namespace mng::port;
void need(bool value) {if(!value)throw std::runtime_error("porting assertion failed");}
template<class F> void rejected(F f) {
    try {f();}catch(const std::runtime_error&){return;}
    throw std::runtime_error("invalid operation accepted");
}
int main(int argc,char** argv) {
    if(argc==2 && std::string(argv[1])=="--debugger") {
        DebugSession debug(true);
        debug.publish("{\"fixture\":true,\"phase\":\"attach_window\"}");
        // Bounded test-fixture window for a DAP controller that attaches before
        // function breakpoints can be installed. Not part of the game runtime.
        std::this_thread::sleep_for(std::chrono::seconds(30));
        debug.publish("{\"fixture\":true,\"phase\":\"checkpoint\"}");
        return 0;
    }
    const std::array<uint8_t,3> abc={'a','b','c'};
    need(sha256(abc)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // Synthetic bytes only. These are not game instructions or patch sites.
    const std::vector<uint8_t> original(32,0x11);
    Identity identity{"fixture","macos-arm64","main","aabb",std::string(64,'a'),sha256(original)};
    Patch first{"first",PatchKind::Compatibility,identity,4,{0x11,0x11,0x11,0x11},{1,2,3,4},"fixture","synthetic test"};
    Patch second=first;second.id="second";second.offset=16;
    std::vector<Patch> patches={first,second};std::vector<std::string> enabled={"first","second"};
    auto bytes=original;
    auto ok=apply_staged(bytes,identity,patches,enabled);
    need(ok.applied.size()==2 && ok.output_sha256!=ok.input_sha256 && bytes[4]==1 && bytes[16]==1);
    need(bytes[0]==0x11 && bytes[20]==0x11);
    need(ok.identity.profile==identity.profile && ok.identity.build_id==identity.build_id);
    need(ok.applied[0].id=="first" && ok.applied[0].module_offset==4 && ok.applied[0].size==4);
    need(ok.applied[0].reason==first.reason && ok.applied[0].evidence==first.evidence);
    need(ok.applied[0].expected_sha256==sha256(first.expected) && ok.applied[0].replacement_sha256==sha256(first.replacement));
    auto fails_unchanged=[&](std::vector<Patch> p, std::vector<std::string> ids=std::vector<std::string>{}) {
        if(ids.empty())ids=enabled;
        auto copy=original;rejected([&]{apply_staged(copy,identity,p,ids);});need(copy==original);
    };
    patches[1].expected[0]=0xff;fails_unchanged(patches);patches[1]=second;
    patches[1].offset=4;fails_unchanged(patches);patches[1]=second;
    patches[1].offset=SIZE_MAX-3;fails_unchanged(patches);patches[1]=second;
    patches[1].identity.build_id="ccdd";fails_unchanged(patches);patches[1]=second;
    patches[1].identity.platform="android-arm64";fails_unchanged(patches);patches[1]=second;
    patches[1].identity.source_sha256=std::string(64,'b');fails_unchanged(patches);patches[1]=second;
    patches[1].kind=PatchKind::Feature;fails_unchanged(patches);patches[1]=second;
    patches[1].kind=PatchKind::Diagnostic;fails_unchanged(patches);patches[1]=second;
    patches[0].kind=PatchKind::Feature;patches[1].kind=PatchKind::Diagnostic;
    bytes=original;rejected([&]{apply_staged(bytes,identity,patches,enabled,true,false);});need(bytes==original);
    auto opted_in=apply_staged(bytes,identity,patches,enabled,true,true);need(opted_in.applied.size()==2);
    patches={first,second};
    fails_unchanged(patches,{"first","missing"});fails_unchanged(patches,{"first","first"});
    bytes=original;need(apply_staged(bytes,identity,patches,{}).applied.empty());need(bytes==original);
    patches[0].identity.build_id="aabb0000";bytes=original;apply_staged(bytes,identity,patches,enabled);
    bytes=original;bytes[30]^=1;auto changed=bytes;
    rejected([&]{apply_staged(bytes,identity,patches,enabled);});need(bytes==changed);
    uint64_t generation;
    {
        DebugSession disabled(false);disabled.publish("ignored");need(!mng_port_debug_descriptor.generation);
        DebugSession debug(true);generation=debug.generation();
        rejected([&]{DebugSession conflict(true);});
        debug.publish("{\"fixture\":true}");
        need(mng_port_debug_descriptor.abi_version==1 && mng_port_debug_descriptor.sequence%2==0);
        need(mng_port_debug_descriptor.generation==generation);
        need(std::string(reinterpret_cast<const char*>(mng_port_debug_descriptor.payload_address),mng_port_debug_descriptor.payload_size)=="{\"fixture\":true}");
        need(!mng_port_debug_open());
    }
    need(!mng_port_debug_descriptor.generation&&!mng_port_debug_descriptor.payload_address);
    auto next=mng_port_debug_open();need(next>generation);
    need(!mng_port_debug_publish(generation,"{}",2));
    need(mng_port_debug_publish(next,"{\"c_adapter\":true}",18));
    mng_port_debug_close(generation);need(mng_port_debug_descriptor.generation==next);
    mng_port_debug_close(next);need(!mng_port_debug_descriptor.generation);
    puts("PASS: identity, atomic patch rejection, opt-in categories, debugger ownership and generations");
}
