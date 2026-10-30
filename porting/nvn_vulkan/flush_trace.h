// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
struct MngFlushSample {uint64_t address{},bytes{},count{};};
extern "C" unsigned mng_nvn_flush_samples(MngFlushSample*,unsigned);
