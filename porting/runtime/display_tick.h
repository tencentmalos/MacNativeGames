// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace mng::port {
// Call under the host display mutex. Missing callbacks advance the sequence by
// elapsed refreshes, so a late frame never waits for an extra complete period.
class DisplayTickClock {
public:
    bool tick(uint64_t frame_ns, double hz) {
        if (!frame_ns || !std::isfinite(hz) || hz < 24 || hz > 360 ||
            (frame_ns_ && frame_ns <= frame_ns_)) return false;
        const auto elapsed = frame_ns_ ? frame_ns - frame_ns_ : 0;
        sequence_ += elapsed && elapsed < 1000000000 && std::abs(hz - hz_) < 1
            ? std::max(uint64_t(1), uint64_t(std::llround(double(elapsed) * hz / 1e9))) : 1;
        frame_ns_ = frame_ns;
        hz_ = hz;
        return true;
    }
    bool step(uint64_t now_ns, unsigned interval, uint64_t& ticks) const {
        if (!interval || !frame_ns_ || now_ns < frame_ns_ || now_ns - frame_ns_ > 100000000) return false;
        const double desired = interval * hz_ / 60.0;
        ticks = uint64_t(std::llround(desired));
        // Non-integral cadences keep the monotonic image-reuse fallback.
        return ticks && std::abs(desired - ticks) < .02;
    }
    uint64_t sequence() const { return sequence_; }
private:
    uint64_t frame_ns_{}, sequence_{};
    double hz_{};
};
}
