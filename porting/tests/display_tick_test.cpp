// SPDX-License-Identifier: MIT
#include "../runtime/display_tick.h"
#include <cassert>
#include <cstdio>
int main() {
    for(unsigned hz : {60u, 90u, 120u}) {
        mng::port::DisplayTickClock clock;
        constexpr uint64_t start=1000000000;
        uint64_t step{};
        assert(!clock.step(start,2,step));
        assert(clock.tick(start,hz));
        assert(clock.step(start,2,step) && step==hz/30);
        const auto previous=clock.sequence();
        assert(clock.tick(start+100000000,hz));
        assert(clock.sequence()>=previous+step); // late: no extra period
        assert(!clock.tick(start+100000000,hz)); // duplicate callback
        assert(!clock.step(start+201000000,2,step)); // stalled display
        assert(!clock.step(start+100000000,0,step));
    }
    mng::port::DisplayTickClock odd;
    uint64_t step{};
    assert(odd.tick(1000000000,90));
    assert(!odd.step(1000000000,1,step)); // 60 FPS on 90 Hz uses fallback
    assert(!odd.tick(1100000000,0));
    puts("PASS: display cadence, missed/duplicate ticks, stale callback and fallback");
}
