// SPDX-License-Identifier: MIT
#include "../runtime/live_ranges.h"
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>
using mng::port::LiveRanges;
int main(){
    LiveRanges ranges;
    auto first=ranges.retain(100,20,LiveRanges::Writeback);
    assert(!ranges.match(80,20) && !ranges.match(120,1));
    assert(ranges.match(119,2)==LiveRanges::Writeback);
    auto alias=ranges.retain(110,20,LiveRanges::Report);
    auto copy=first;first.reset();
    assert(ranges.match(115,1)==(LiveRanges::Writeback|LiveRanges::Report));
    copy.reset();assert(!ranges.match(100,10));
    assert(ranges.match(115,1)==LiveRanges::Report);
    alias.reset();assert(!ranges.match(100,40));
    auto reuse=ranges.retain(100,20,LiveRanges::Event);
    assert(ranges.match(100,1)==LiveRanges::Event);reuse.reset();
    LiveRanges bounded(1);
    auto a=bounded.retain(10,10,LiveRanges::Image),b=bounded.retain(50,10,LiveRanges::Report);
    assert(bounded.match(1000,1)==LiveRanges::Unknown);
    a.reset();assert(bounded.match(1000,1)==LiveRanges::Unknown);
    b.reset();assert(!bounded.match(1000,1));
    auto bad=ranges.retain(UINT64_MAX-1,4,LiveRanges::Image);
    assert(ranges.match(200,1)==LiveRanges::Unknown);bad.reset();
    assert(ranges.match(UINT64_MAX-1,4)==LiveRanges::Unknown);
    std::atomic<unsigned> ready=0;std::atomic<bool> release=false;
    std::vector<std::thread> threads;
    for(unsigned i=0;i<8;++i)threads.emplace_back([&,i]{
        auto lease=ranges.retain(1000+i*16,16,LiveRanges::Writeback);
        ++ready;while(!release.load())std::this_thread::yield();
    });
    while(ready.load()!=8)std::this_thread::yield();
    for(unsigned i=0;i<128;++i)assert(ranges.match(1000+i,1)==LiveRanges::Writeback);
    release=true;for(auto &thread:threads)thread.join();
    assert(!ranges.match(1000,128) && ranges.stats().ranges==0);
    LiveRanges::Lease survivor;
    {LiveRanges transient;survivor=transient.retain(1,1,LiveRanges::Event);}
    survivor.reset(); // A lease may outlive its registry facade.
}
