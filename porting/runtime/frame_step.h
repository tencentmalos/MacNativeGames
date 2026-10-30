// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
namespace mng::port {
// MacNative BOTW's five-sample clock; pauses do not become catch-up simulation.
// Clamp the average, not each sample, to preserve time under alternating cadence.
class FrameStep {
public:
    double seconds{1.0/60.0}, elapsed{};
    uint32_t pad_tick{1};
    void advance(uint64_t now) {
        elapsed=0;
        if(last_ && now>last_) {
            const double dt=(now-last_)*1e-9;
            if(dt>=.001 && dt<=.25) {
                elapsed=dt; samples_[next_++%5]=dt; count_=std::min(count_+1,5u);
                double sum=0;for(unsigned i=0;i<count_;++i)sum+=samples_[i];
                seconds=std::min(sum/count_,1.0/15.0);
            } else {count_=next_=0;seconds=1.0/60.0;}
        } else if(last_) {count_=next_=0;seconds=1.0/60.0;}
        last_=now;
        phase_+=double(float(seconds))*30.0;pad_tick=0;
        if(phase_>=1.0){pad_tick=1;phase_-=1.0;if(phase_>=1.0)phase_=0;}
    }
    uint64_t bits() const {
        const float s=float(seconds);
        return uint64_t(std::bit_cast<uint32_t>(s*30.0f)) |
            (uint64_t(std::bit_cast<uint32_t>(s))<<32);
    }
private:
    std::array<double,5> samples_{};
    unsigned count_{},next_{};
    uint64_t last_{};
    double phase_{};
};
}
