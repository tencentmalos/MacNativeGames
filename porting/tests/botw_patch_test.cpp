// SPDX-License-Identifier: MIT
#include "exclusive_acquire.h"
#include "render_resolution.h"
#include "realtime.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
using namespace mng::botw;
void need(bool v) {if(!v)throw std::runtime_error("exclusive window test failed");}
int main(int argc,char** argv) {
    for(const unsigned hz:{30u,60u,95u,120u,240u}) {
        mng::port::FrameStep clock;unsigned ticks=0;double elapsed=0,game=0;
        for(unsigned i=0;i<10*hz;++i){clock.advance(1000000000ull+uint64_t(i)*1000000000ull/hz);
            ticks+=clock.pad_tick;if(clock.elapsed){elapsed+=clock.elapsed;game+=clock.seconds;}}
        need(std::abs(elapsed-game)<.001 && ticks>=298 && ticks<=300);
    }
    mng::port::FrameStep jitter;double game=0,real=0;uint64_t now=1000000000;
    for(int i=0;i<1000;++i){now+=i%2?24000000:9333333;jitter.advance(now);
        if(i>10){game+=jitter.seconds;real+=jitter.elapsed;}}
    need(std::abs(game-real)<.02);
    jitter.advance(now+1000000000);need(jitter.seconds==1.0/60.0 && jitter.elapsed==0);
    if(argc>=2) {
        // Optional private verified fixture; no game bytes enter the repository.
        std::ifstream file(argv[1],std::ios::binary);
        std::vector<uint8_t> image((std::istreambuf_iterator<char>(file)),{});
        const mng::port::Identity identity{"botw160","android-arm64","main",
            "8e9978d50bdd20b4c8395a106c27ffde00000000000000000000000000000000",
            "15cfca7b89348956f85d945fade2e215a6af5991ed1071e181f97ca72f7ae20b",
            "cf5d57a21cd85a627de7c9c500957d971b81b6aebbc3360ed78edbe350b41884"};
        auto wrong=identity;wrong.platform="macos-arm64";bool rejected=false;
        try{apply_exclusive_acquire(image,wrong,34773924,47644672);}catch(const std::runtime_error&){rejected=true;}
        need(rejected && mng::port::sha256(image)==identity.image_sha256);
        const auto receipt=apply_exclusive_acquire(image,identity,34773924,47644672);
        need(receipt.applied.size()==18312 && receipt.output_sha256=="60474b0e3056daf4aee06c875de30bb001310d934437da0c4e99bdb76ca05007");
        rejected=false;
        try{apply_exclusive_acquire(image,identity,34773924,47644672);}catch(const std::runtime_error&){rejected=true;}
        need(rejected && mng::port::sha256(image)==receipt.output_sha256);
        // Independently matched in the exact Android rewrite: the handheld
        // branch retains 1280.0 for both canvas and base scene widths. Do not
        // inherit the Apple mobile 960-pixel scene patch on Android.
        const std::array<uint8_t,12> handheld{
            0x08,0x94,0xa8,0xd2,0x08,0x94,0xe8,0xf2,0x88,0x4b,0x09,0xf9};
        need(std::equal(handheld.begin(),handheld.end(),image.begin()+0x18216c8));
        auto resolution_identity=identity;resolution_identity.image_sha256=receipt.output_sha256;
        auto original1080=image;
        const size_t timing_page=(image.size()+0x3fff)&~size_t(0x3fff);
        auto timing_image=image;auto wrong_timing=resolution_identity;wrong_timing.build_id="aabb";
        rejected=false;try{apply_realtime(timing_image,wrong_timing,timing_page);}catch(const std::runtime_error&){rejected=true;}
        need(rejected && timing_image==image);
        rejected=false;try{apply_realtime(timing_image,resolution_identity,timing_page+4096);}catch(const std::runtime_error&){rejected=true;}
        need(rejected && timing_image==image);
        auto corrupt=timing_image;corrupt[0x126e490]^=1;
        rejected=false;try{apply_realtime(corrupt,resolution_identity,timing_page);}catch(const std::runtime_error&){rejected=true;}
        need(rejected && corrupt[0x126e490]==(image[0x126e490]^1));
        const auto timing=apply_realtime(timing_image,resolution_identity,timing_page);
        need(timing.applied.size()==60);
        printf("timing_output_sha256=%s page=%zx\n",timing.output_sha256.c_str(),timing_page);
        rejected=false;try{apply_realtime(timing_image,resolution_identity,timing_page);}catch(const std::runtime_error&){rejected=true;}
        need(rejected && mng::port::sha256(timing_image)==timing.output_sha256);
        if(argc==3){std::ofstream out(argv[2],std::ios::binary);out.write(reinterpret_cast<char*>(timing_image.data()),timing_image.size());}
        timing_image.resize(timing_page+16384);
        {
            RealtimeSession session(timing_image.data(),timing_page);double seconds=0;
            need(session.update(1000000000,&seconds)==1);
            uint64_t movie=1;memcpy(timing_image.data()+0x2cbef40,&movie,8);
            need(session.update(1033333333,&seconds)==2);
            movie=0;memcpy(timing_image.data()+0x2cbef40,&movie,8);
            need(session.update(1050000000,&seconds)==1);
        }
        auto invalid=resolution_identity;invalid.build_id="aabb";rejected=false;
        try{apply_render_1080(image,invalid);}catch(const std::runtime_error&){rejected=true;}
        need(rejected && image==original1080);
        const auto resolution=apply_render_1080(image,resolution_identity);
        need(resolution.applied.size()==1 && image[0x1821675]==0x9e);
        need(std::equal(handheld.begin(),handheld.end(),image.begin()+0x18216c8));
        auto docked_identity=resolution_identity;docked_identity.image_sha256=resolution.output_sha256;
        auto docked_timing=image;const auto docked_result=apply_realtime(docked_timing,docked_identity,timing_page);
        need(docked_result.applied.size()==60);
        printf("docked_timing_output_sha256=%s\n",docked_result.output_sha256.c_str());
        printf("resolution_output_sha256=%s\n",resolution.output_sha256.c_str());
        rejected=false;try{apply_render_1080(image,resolution_identity);}catch(const std::runtime_error&){rejected=true;}
        need(rejected && mng::port::sha256(image)==resolution.output_sha256);
        puts("PASS: private Android fixture, exact output, wrong-platform atomic rejection and double-apply rejection");
        return 0;
    }
    std::vector<uint8_t> code(80);
    auto put=[&](size_t off,uint32_t w){for(unsigned i=0;i<4;++i)code[off+i]=uint8_t(w>>(i*8));};
    put(0,0x885f7c20);put(64,0x88027c23); // ldxr w0,[x1]; stxr w2,w3,[x1]
    auto w=exclusive_windows(code);need(w.size()==1 && w[0].store==64 && w[0].replacement==0x885ffc20);
    put(64,0);put(68,0x88027c23);need(exclusive_windows(code).empty());
    put(4,0x88027c43);need(exclusive_windows(code).empty()); // different base x2
    put(4,0x88027c23);put(0,0x885ffc20);need(exclusive_windows(code).empty()); // already acquire
    put(0,0xc87f0440);put(4,0xc8220c44); // ldxp x0,x1,[x2]; stxp w2,x4,x3,[x2]
    w=exclusive_windows(code);need(w.size()==1 && w[0].replacement==0xc87f8440);
    auto original=code;bool rejected=false;
    try {mng::port::Identity wrong{};apply_exclusive_acquire(code,wrong,code.size(),0);}
    catch(const std::runtime_error&){rejected=true;}
    need(rejected && code==original);
    puts("PASS: exclusive opcodes, same base, bounded window, acquire preservation and title rejection");
}
