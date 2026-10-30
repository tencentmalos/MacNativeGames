// SPDX-License-Identifier: MIT
// Adapted from games/totk/runtime/game_patches_botw.c (MacNativeGames).
// Only the independently checked BOTW 1.6.0 Android rewrite is accepted.
#include "realtime.h"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <stdexcept>
namespace mng::botw {
namespace {
std::mutex callback_mutex;
RealtimeSession* active{};
constexpr size_t movie_word=0x2cbef40;
uint32_t adrp_reg(unsigned rd, uint64_t site, uint64_t target) {
    int64_t pages=int64_t(target>>12)-int64_t(site>>12);
    if(pages<-(1<<20) || pages>=(1<<20))throw std::runtime_error("BOTW timing page outside ADRP range");
    uint32_t imm=uint32_t(pages)&0x1fffff;
    return 0x90000000u|((imm&3u)<<29)|((imm>>2)<<5)|rd;
}
std::vector<uint8_t> encoded(uint32_t w) {
    return {uint8_t(w),uint8_t(w>>8),uint8_t(w>>16),uint8_t(w>>24)};
}
static const uint32_t pad_loop_shipped[] = {
    0xb9410268u, 0x7100051fu, 0xb901166bu, 0xb900127fu, 0x5400044bu, 0x2a1f03e8u, 0xaa1f03e9u, 0x320003eau, 0x14000002u, 0xb941166bu,
    0x1ac9214cu, 0x6a0c017fu, 0x54000180u, 0x8b09026eu, 0x394341cdu, 0x8b090a6bu, 0x340001edu, 0xb940316fu, 0x3942c1ceu, 0x6b0e01eeu,
    0x540000e1u, 0x2a0c0108u, 0xb9001268u, 0x14000008u, 0x8b090a6bu, 0xb900317fu, 0x14000008u, 0x54000089u, 0x1acd09cfu, 0x1b0db9edu,
    0x34fffeedu, 0xb940316cu, 0x1100058cu, 0xb900316cu, 0xb981026bu, 0x91000529u, 0xeb0b013fu, 0x54fffc8bu,
};
static const uint32_t pad_loop_ticked[] = {
    0xb9410268u, /* ldr   w8, [x19, #0x100]   (button count) */
    0x7100051fu, /* cmp   w8, #1 */
    0xb901166bu, /* str   w11, [x19, #0x114]  (hold) */
    0xb900127fu, /* str   wzr, [x19, #0x10]   (repeat = 0) */
    0x5400044bu, /* b.lt  end */
    0x2a1f03e8u, /* mov   w8, wzr */
    0xaa1f03e9u, /* mov   x9, xzr */
    0,           /* adrp  x16, <data page> */
    0xb9400a10u, /* ldr   w16, [x16, #8]      (tick) */
    0x5280002au, /* mov   w10, #1 */
    0x1ac9214cu, /* loop: lsl w12, w10, w9 */
    0x8b090a71u, /* add   x17, x19, x9, lsl #2 */
    0x6a0c017fu, /* tst   w11, w12 */
    0x54000180u, /* b.eq  released */
    0x34000290u, /* cbz   w16, next           (no tick: count and repeat unchanged) */
    0x8b09026eu, /* add   x14, x19, x9 */
    0x394341cdu, /* ldrb  w13, [x14, #0xd0]   (pulse) */
    0x340001cdu, /* cbz   w13, count */
    0xb940322fu, /* ldr   w15, [x17, #0x30]   (count) */
    0x3942c1ceu, /* ldrb  w14, [x14, #0xb0]   (delay) */
    0x6b0e01eeu, /* subs  w14, w15, w14 */
    0x540000c1u, /* b.ne  past */
    0x2a0c0108u, /* repeat: orr w8, w8, w12 */
    0xb9001268u, /* str   w8, [x19, #0x10] */
    0x14000007u, /* b     count */
    0xb900323fu, /* released: str wzr, [x17, #0x30] */
    0x14000008u, /* b     next */
    0x54000089u, /* past: b.ls count */
    0x1acd09cfu, /* udiv  w15, w14, w13 */
    0x1b0db9edu, /* msub  w13, w15, w13, w14 */
    0x34ffff0du, /* cbz   w13, repeat */
    0xb940322cu, /* count: ldr w12, [x17, #0x30] */
    0x1100058cu, /* add   w12, w12, #1 */
    0xb900322cu, /* str   w12, [x17, #0x30] */
    0xb981026fu, /* next: ldrsw x15, [x19, #0x100] */
    0x91000529u, /* add   x9, x9, #1 */
    0xeb0f013fu, /* cmp   x9, x15 */
    0x54fffcabu, /* b.lt  loop */
};
static const uint32_t menu_cd_shipped[] = { 0x8b0a136au, 0x5289400bu, 0x72a000cbu, 0x8b0b014bu, 0xb940016cu, 0x37f80accu, 0x5100058du, 0xb900016du, 0x35000a6cu };
static const uint32_t menu_cd_ticked[] = { 0x8b0a136au, 0x9141914bu, 0, 0xb94009adu, 0x34000aedu, 0xb94a016cu, 0x5100058du, 0xb90a016du, 0x35000a6cu };
}
port::PatchReceipt apply_realtime(std::span<uint8_t> image,const port::Identity& identity,size_t page_offset) {
    if(image.size()!=48355632 || page_offset!=((image.size()+0x3fff)&~size_t(0x3fff)))
        throw std::runtime_error("BOTW timing image/page layout mismatch");
    const bool docked=identity.image_sha256=="d48e4b5eb443a5a9f8f370101d4fd7182a36830a4b87efa5a6c2be9edff025ea";
    const port::Identity pin{"botw160","android-arm64","main","8e9978d50bdd20b4c8395a106c27ffde",
        "15cfca7b89348956f85d945fade2e215a6af5991ed1071e181f97ca72f7ae20b",
        docked?"d48e4b5eb443a5a9f8f370101d4fd7182a36830a4b87efa5a6c2be9edff025ea":
            "60474b0e3056daf4aee06c875de30bb001310d934437da0c4e99bdb76ca05007"};
    static const struct {uint32_t off,word;} check[]={
        {0x1c0120cu,0x1e202020u},
        {0x1c01210u,0x3900a008u},
        {0x1c01214u,0x54000060u},
        {0x1c01218u,0xbd003c01u},
        {0x1821564u,0xf9400108u},
        {0x1821568u,0xb9400108u},
        {0x182156cu,0x7100011fu},
        {0x1821570u,0x1a9f1508u},
        {0x1821574u,0x1e230100u},
        {0x1821578u,0x1e2c100bu},
        {0x182157cu,0xb0006f68u},
        {0x1821580u,0xbd409109u},
        {0x1821584u,0xbd435ee1u},
        {0x1821588u,0x1e2b0800u},
        {0x182158cu,0x1e290800u},
        {0x18522ecu,0xb9400108u},
        {0x18522f0u,0x7100011fu},
        {0x18522f4u,0x1a9f1508u},
        {0x18522f8u,0x1e230100u},
        {0x18522fcu,0x1e2c1001u},
        {0x1852300u,0x1e210800u},
        {0x1852304u,0xbd0b22a0u},
        {0x161847cu,0x1e290908u},
        {0x1618480u,0xbd400109u},
        {0x1618484u,0xb94c3680u},
        {0x1600e70u,0xd000b5e8u},
        {0x1600e74u,0xf907a115u},
        {0x1a4d6ecu,0xb0009388u},
        {0x1a4d6f0u,0xf907a11fu},

    };
    // Validate neighbors and movie producer/clear instructions before any writes.
    for(const auto& c:check){uint32_t w;memcpy(&w,image.data()+c.off,4);
        if(w!=c.word)throw std::runtime_error("BOTW timing instruction guard mismatch");}
    uint64_t movie;memcpy(&movie,image.data()+movie_word,8);
    if(movie)throw std::runtime_error("BOTW timing requires an unrelocated, inactive movie slot");
    const uint32_t nop=0xd503201f;
    const struct {uint32_t off,word;} put[]={
        {0x1c0120cu, adrp_reg(9, 0x1c0120c, page_offset)}, {0x1c01214u, 0xbd400121u},   /* ldr s1, [x9] */
        {0x1821568u, adrp_reg(8, 0x1821568, page_offset)}, {0x182156cu, 0xbd400100u},   /* ldr s0, [x8] */
        {0x1821570u, nop}, {0x1821574u, nop}, {0x1821588u, nop},
        {0x18522ecu, adrp_reg(8, 0x18522ec, page_offset)}, {0x18522f0u, 0xbd400100u},   /* ldr s0, [x8] */
        {0x18522f4u, nop}, {0x18522f8u, nop}, {0x1852300u, nop},
        {0x1618480u, 0x1e2e1009u},   /* fmov s9, #1.0: six physics substeps per frame at any frame rate */
    };
    std::vector<port::Patch> patches;std::vector<std::string> selected;
    auto add=[&](uint32_t off,uint32_t original,uint32_t replacement,const char* group){
        std::string id=std::string(group)+"."+std::to_string(off);
        selected.push_back(id);
        patches.push_back({id,port::PatchKind::Feature,pin,off,encoded(original),encoded(replacement),
            "Measured game time with stable UI, physics and controller repeat cadence",
            "MacNative BOTW160; verified whole Android rewrite, original words, Unicorn execution and movie store/clear ADRP targets"});
    };
    for(const auto& p:put){
        for(const auto& c:check)if(c.off==p.off){add(p.off,c.word,p.word,"botw160.realtime-vfr");break;}
    }
    for(unsigned i=0;i<38;++i)add(0x126e490+4*i,pad_loop_shipped[i],
        i==7?adrp_reg(16,0x126e490+4*i,page_offset):pad_loop_ticked[i],"botw160.pad-repeat");
    for(unsigned i=0;i<9;++i)add(0x1214474+4*i,menu_cd_shipped[i],
        i==2?adrp_reg(13,0x1214474+4*i,page_offset):menu_cd_ticked[i],"botw160.pad-repeat");
    return port::apply_staged(image,identity,patches,selected,true);
}
RealtimeSession::RealtimeSession(uint8_t* image,size_t page_offset):image_(image),page_(image+page_offset) {
    std::lock_guard lock(callback_mutex);
    if(active)throw std::runtime_error("BOTW timing owner already active");
    __atomic_store_n(reinterpret_cast<uint64_t*>(page_),clock_.bits(),__ATOMIC_RELEASE);
    __atomic_store_n(reinterpret_cast<uint32_t*>(page_+8),1u,__ATOMIC_RELEASE);
    active=this;
}
RealtimeSession::~RealtimeSession(){std::lock_guard lock(callback_mutex);if(active==this)active=nullptr;}
int RealtimeSession::update(uint64_t now,double* seconds) {
    clock_.advance(now);
    __atomic_store_n(reinterpret_cast<uint64_t*>(page_),clock_.bits(),__ATOMIC_RELEASE);
    __atomic_store_n(reinterpret_cast<uint32_t*>(page_+8),clock_.pad_tick,__ATOMIC_RELEASE);
    const bool movie=__atomic_load_n(reinterpret_cast<uint64_t*>(image_+movie_word),__ATOMIC_RELAXED)!=0;
    if(movie!=movie_){movie_=movie;fprintf(stderr,"[botw] movie=%d target_fps=%d\n",movie, movie?30:60);}
    if(seconds)*seconds=clock_.seconds;
    if(!report_ns_)report_ns_=now;
    if(clock_.elapsed){++frames_;real_seconds_+=clock_.elapsed;game_seconds_+=clock_.seconds;}
    if(now-report_ns_>=30000000000ull){
        fprintf(stderr,"[botw] realtime frames=%llu real_s=%.6f game_s=%.6f ratio=%.6f step_ms=%.3f target_fps=%d\n",
            (unsigned long long)frames_,real_seconds_,game_seconds_,real_seconds_?game_seconds_/real_seconds_:0,
            clock_.seconds*1000,movie?30:60);
        report_ns_=now;frames_=0;real_seconds_=game_seconds_=0;
    }
    return movie?2:1;
}
}
extern "C" int mng_game_frame_step(uint64_t now,double* seconds) {
    std::lock_guard lock(mng::botw::callback_mutex);
    return mng::botw::active?mng::botw::active->update(now,seconds):0;
}
