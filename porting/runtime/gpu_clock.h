// SPDX-License-Identifier: MIT
#pragma once
#include <cmath>
#include <cstdint>
#include <limits>

namespace mng::port {
// GPU query ticks have a device-specific epoch. Convert relative to a measured
// host anchor, including wraparound, before exposing them to the guest clock.
class GpuClock {
public:
    bool anchor(uint64_t gpu, uint64_t host, double period, unsigned bits) {
        if (!bits || bits > 64 || !std::isfinite(period) || period <= 0) return false;
        bits_ = bits;
        mask_ = bits == 64 ? UINT64_MAX : (uint64_t(1) << bits) - 1;
        gpu_ = gpu & mask_;
        host_ = host;
        period_ = period;
        return true;
    }
    bool nanoseconds(uint64_t gpu, uint64_t& out) const {
        if (!bits_) return false;
        const uint64_t wrapped = (gpu - gpu_) & mask_;
        const __int128 delta = wrapped & (uint64_t(1) << (bits_ - 1))
            ? __int128(wrapped) - (__int128(1) << bits_) : __int128(wrapped);
        const long double value = static_cast<long double>(host_) +
            static_cast<long double>(delta) * period_;
        if (value < 0 || value >= std::ldexp(1.0L,64)) return false;
        out = static_cast<uint64_t>(value);
        return true;
    }
    static uint64_t guestTicks(uint64_t ns) {
        return uint64_t((__uint128_t(ns) * 12) / 625);
    }
private:
    unsigned bits_{};
    uint64_t gpu_{}, host_{}, mask_{};
    long double period_{};
};
}
