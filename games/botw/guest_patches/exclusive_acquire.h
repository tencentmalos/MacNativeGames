// SPDX-License-Identifier: MIT
#pragma once
#include "port_runtime.h"

namespace mng::botw {
struct ExclusiveWindow { size_t load, store; uint32_t original, replacement; };
// Offsets are relative to this span. Pure decoding, no writes or title policy.
std::vector<ExclusiveWindow> exclusive_windows(std::span<const uint8_t> code);
// Exact Android BOTW 1.6.0 rewrite, private RW staging, before relocations.
port::PatchReceipt apply_exclusive_acquire(std::span<uint8_t> image,
    const port::Identity& identity, size_t text_end, size_t patch_offset);
}
