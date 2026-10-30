// SPDX-License-Identifier: MIT
#include "../runtime/frontlock_stats.h"
#include <cassert>
#include <thread>
#include <vector>
int main(){
    using mng::port::FrontLockStats;FrontLockStats stats;FrontLockStats::Snapshot out;
    static const char waiter[]="waiter",holder[]="holder";
    assert(!stats.token());stats.record(0,waiter,1,holder,999);
    assert(stats.control(1,out));const auto token=stats.token();
    std::vector<std::thread> workers;
    for(int i=0;i<8;++i)workers.emplace_back([&]{for(int j=0;j<1000;++j)stats.record(token,waiter,1,holder,17);});
    for(auto &w:workers)w.join();assert(stats.control(-1,out));
    assert(out.size==1 && out.rows[0].count==8000 && out.rows[0].total_ns==136000 && out.rows[0].max_ns==17);
    assert(stats.control(2,out));stats.record(token,waiter,1,holder,1);assert(stats.control(-1,out) && !out.size);
    const auto next=stats.token();for(unsigned h=0;h<130;++h)stats.record(next,waiter,h,holder,h+1);
    assert(stats.control(0,out));assert(out.size==128 && out.other_count==2 && out.other_ns==259 && out.other_max_ns==130);
    stats.record(next,waiter,1,holder,99);assert(!stats.token());
    assert(stats.control(-1,out) && out.rows[1].count==1);
}
