// SPDX-License-Identifier: MIT
#include "exclusive_acquire.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

namespace mng::botw {
namespace {
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
uint32_t word(std::span<const uint8_t> bytes, size_t offset) {
    return uint32_t(bytes[offset]) | uint32_t(bytes[offset+1])<<8 |
           uint32_t(bytes[offset+2])<<16 | uint32_t(bytes[offset+3])<<24;
}
std::vector<uint8_t> encoded(uint32_t w) {
    return {uint8_t(w),uint8_t(w>>8),uint8_t(w>>16),uint8_t(w>>24)};
}
}
std::vector<ExclusiveWindow> exclusive_windows(std::span<const uint8_t> code) {
    require(code.size()%4==0,"Unaligned exclusive-window range");
    std::vector<ExclusiveWindow> result;
    for (size_t i=0;i<code.size();i+=4) {
        const auto load=word(code,i);
        // Same opcode/register rule as the verified Mac BOTW implementation.
        // Already-acquiring loads do not match and are never weakened.
        if ((load&0x3ffffc00u)!=0x085f7c00u && (load&0xbfff8000u)!=0x887f0000u) continue;
        for (size_t j=i+4;j<code.size() && j-i<=64;j+=4) {
            const auto store=word(code,j);
            if (((store&0x3fe07c00u)==0x08007c00u || (store&0xbfe00000u)==0x88200000u) &&
                ((load>>5)&31)==((store>>5)&31)) {
                result.push_back({i,j,load,load|0x8000u});break;
            }
        }
    }
    return result;
}
port::PatchReceipt apply_exclusive_acquire(std::span<uint8_t> image,
    const port::Identity& identity, size_t text_end, size_t patch_offset) {
    const port::Identity pin{"botw160","android-arm64","main",
        "8e9978d50bdd20b4c8395a106c27ffde",
        "15cfca7b89348956f85d945fade2e215a6af5991ed1071e181f97ca72f7ae20b",
        "cf5d57a21cd85a627de7c9c500957d971b81b6aebbc3360ed78edbe350b41884"};
    require(image.size()==48355632 && text_end==34773924 && patch_offset==47644672,
        "BOTW exclusive-acquire layout mismatch");
    require(port::sha256(image)==pin.image_sha256,"BOTW exclusive-acquire rewrite mismatch");
    const std::array ranges={image.first(text_end),image.subspan(patch_offset)};
    const std::array<size_t,2> bases={0,patch_offset}, counts={18249,63};
    std::vector<port::Patch> patches;
    std::vector<std::string> selected;
    auto candidate=std::vector<uint8_t>(image.begin(),image.end());
    for (size_t range=0;range<ranges.size();++range) {
        const auto windows=exclusive_windows(ranges[range]);
        require(windows.size()==counts[range],"BOTW exclusive-acquire window count mismatch");
        for (const auto& window:windows) {
            const auto offset=bases[range]+window.load;
            const auto id="botw160.exclusive-acquire."+std::to_string(offset);
            auto replacement=encoded(window.replacement);
            std::copy(replacement.begin(),replacement.end(),candidate.begin()+offset);
            patches.push_back({id,port::PatchKind::Compatibility,pin,offset,encoded(window.original),replacement,
                "Acquire ordering for a matched exclusive synchronization window",
                "game_patches_botw.c:excl_acquire; exact Android rewrite; matching store at main+"+
                    std::to_string(bases[range]+window.store)});
            selected.push_back(id);
        }
    }
    // The whole transformed image is pinned as well as every original word.
    // Reject a changed scanner before the common executor can write anything.
    require(port::sha256(candidate)=="60474b0e3056daf4aee06c875de30bb001310d934437da0c4e99bdb76ca05007",
        "BOTW exclusive-acquire output mismatch");
    return port::apply_staged(image,identity,patches,selected);
}
}
