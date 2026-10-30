// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>
#include "performance_hint_queue.h"
namespace mng::port {
// Debugbus control never calls the Android service or acquires renderer locks.
std::string performance_hint_control(int enabled);
uint64_t performance_hint_begin();
void performance_hint_end(HintLane lane, uint64_t begin_ns);
void performance_hint_report(HintLane lane, uint64_t elapsed_ns);
}
