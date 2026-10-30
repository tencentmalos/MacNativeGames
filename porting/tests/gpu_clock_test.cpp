// SPDX-License-Identifier: MIT
#include "../runtime/gpu_clock.h"
#include <cassert>
#include <cstdio>
#include <initializer_list>
int main() {
    mng::port::GpuClock clock; uint64_t ns{};
    assert(!clock.nanoseconds(1,ns));
    // The Odin3 failure: GPU uptime and guest uptime differ by days. Absolute
    // ticks must not leak through, while relative query durations stay intact.
    constexpr uint64_t gpu=10062352438078, host=247611040000000;
    assert(clock.anchor(gpu,host,625.0/12,48));
    assert(clock.nanoseconds(gpu,ns) && ns==host);
    assert(clock.nanoseconds(gpu+192000,ns) && ns==host+10000000);
    assert(clock.nanoseconds(gpu-192000,ns) && ns==host-10000000);
    for(unsigned bits:{32u,48u,64u}) {
        const auto mask=bits==64?UINT64_MAX:(uint64_t(1)<<bits)-1;
        assert(clock.anchor(mask-4,1000000,10, bits));
        assert(clock.nanoseconds(7,ns) && ns==1000120);
        assert(clock.anchor(7,1000000,10,bits));
        assert(clock.nanoseconds(mask-4,ns) && ns==999880);
    }
    assert(!clock.anchor(0,0,1,0)); assert(!clock.anchor(0,0,0,64));
    assert(clock.anchor(100,1,1,64)); assert(!clock.nanoseconds(0,ns));
    assert(mng::port::GpuClock::guestTicks(1000000000)==19200000);
    assert(mng::port::GpuClock::guestTicks(UINT64_MAX)==uint64_t((__uint128_t(UINT64_MAX)*12)/625));
    puts("PASS: GPU/guest epoch, durations, signed wrap 32/48/64, overflow-safe guest ticks");
}
