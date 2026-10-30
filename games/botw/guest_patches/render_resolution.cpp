// SPDX-License-Identifier: MIT
#include "render_resolution.h"
#include <array>
#include <stdexcept>
namespace mng::botw {
port::PatchReceipt apply_render_1080(std::span<uint8_t> image,const port::Identity& identity) {
    const port::Identity pin{"botw160","android-arm64","main",
        "8e9978d50bdd20b4c8395a106c27ffde",
        "15cfca7b89348956f85d945fade2e215a6af5991ed1071e181f97ca72f7ae20b",
        "60474b0e3056daf4aee06c875de30bb001310d934437da0c4e99bdb76ca05007"};
    if(image.size()!=48355632)throw std::runtime_error("BOTW resolution layout mismatch");
    // Independently checked against the pinned Android rewrite's complete image
    // and display-mode instruction triplet. Set the render width to the canvas
    // width; mip/half-size targets continue to be allocated by the guest.
    const port::Patch patch{"botw160.render-1080",port::PatchKind::Feature,pin,0x1821670,
        {0x08,0x9e,0xa8,0xd2,0x08,0x99,0xe8,0xf2,0x88,0x4b,0x09,0xf9},
        {0x08,0x9e,0xa8,0xd2,0x08,0x9e,0xe8,0xf2,0x88,0x4b,0x09,0xf9},
        "Docked scene width equals the 1920-pixel canvas, with internal scale 1x",
        "MacNative patch_render_1080; exact BOTW160 Android image SHA and checked instruction triplet"};
    const std::array selected{patch.id};
    auto result=port::apply_staged(image,identity,{&patch,1},selected,true);
    if(result.output_sha256!="d48e4b5eb443a5a9f8f370101d4fd7182a36830a4b87efa5a6c2be9edff025ea")
        throw std::runtime_error("BOTW resolution output digest mismatch");
    return result;
}
}
