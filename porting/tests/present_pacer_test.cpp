// SPDX-License-Identifier: MIT
#include "../../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_pace.h"
#include "../runtime/present_period.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
int main() {
    using mng::port::present_period;
    assert(present_period(2,1,120)==1000000000ull/120);
    assert(present_period(2,1,60)==1000000000ull/60);
    assert(present_period(1,2,120)==2000000000ull/60);
    assert(present_period(2,0,120)==2000000000ull/60);
    assert(present_period(0,0,120)==0);
    assert(present_period(2,1,0)==1000000000ull/60);
    // Simulate the production image-reuse protocol with 2/3/4 images. Present
    // never blocks, bounded queue depth does not multiply the steady frame rate.
    constexpr uint64_t period=2000000000ull/60;
    for(unsigned images=2;images<=4;++images){
        nvn_pacer pacer{};uint64_t available[4]{},now=1000000000,begin{};
        for(unsigned frame=0;frame<=1100;++frame){
            auto index=frame%images;now=std::max(now,available[index]);now+=8000000;
            auto slot=now+nvn_pace(&pacer,now,period);available[index]=slot+period;
            if(frame==100)begin=now;
        }
        assert(now-begin==1000*period);
    }
    nvn_pacer pacer{1000000000};
    assert(nvn_pace(&pacer,1001000000,period)==period-1000000);
    const auto target=pacer.target;
    assert(nvn_pace(&pacer,target+period+1000000,period)==0 && pacer.target==target+period);
    assert(nvn_pace(&pacer,target+10*period,period)==0 && pacer.target==target+10*period);
    assert(nvn_pace(&pacer,target+11*period,0)==0 && pacer.target==target+11*period);
    puts("PASS: bounded 2/3/4-image pacing at 30 FPS, late phase, stall recovery, interval zero");
}
