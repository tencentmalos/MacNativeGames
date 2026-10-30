// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
namespace mng::port {
// Only a real-time game policy may override the requested NVN interval.
// Movie/30 FPS policy remains independent of the gameplay ceiling.
constexpr uint64_t present_period(int interval,int game_policy,int gameplay_hz) {
    if(game_policy==1 && gameplay_hz>=30 && gameplay_hz<=240)
        return 1000000000ull/unsigned(gameplay_hz);
    return uint64_t(game_policy?game_policy:interval)*1000000000ull/60;
}
}
