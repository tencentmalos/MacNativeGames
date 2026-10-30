// SPDX-License-Identifier: MIT
#pragma once
#include "port_runtime.h"
namespace mng::botw {
// Applies only after exclusive-acquire, before relocation, for explicit 1080p.
port::PatchReceipt apply_render_1080(std::span<uint8_t>,const port::Identity&);
}
