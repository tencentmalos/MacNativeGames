// SPDX-License-Identifier: MIT
#pragma once
// Include after nvn_tex.h. MacNativeGames 3c482a7 and Citron's NVN layout
// agree: LINEAR_RENDER_TARGET alone selects pitch storage. BOTW 1.6.0 uses
// it for CPU-backed R16 height tiles with an explicit 512-byte row stride.
static inline int mng_linear_texture(const nvn_texdesc *d) {
    return (d->flags & 0x50) != 0;
}
static inline size_t mng_linear_stride(const nvn_texdesc *d) {
    if (d->stride) return d->stride;
    int bw, bh;
    const int bpp = nvn_fmt_bpb(d->format, &bw, &bh);
    const size_t alignment = (d->flags & 0x40) ? 128 : 32;
    return ((size_t)((d->width + bw - 1) / bw) * bpp + alignment - 1) & ~(alignment - 1);
}
