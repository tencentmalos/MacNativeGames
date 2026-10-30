// SPDX-License-Identifier: MIT
#include "../runtime/conservative_ranges.h"
#include <cassert>
#include <thread>
#include <vector>
int main() {
    mng::port::ConservativeRanges r;
    r.include(100,10);r.include(120,10);
    assert(!r.overlaps(110,10));assert(r.overlaps(109,2));assert(r.overlaps(119,2));
    r.include(110,10);assert(r.overlaps(110,10));assert(!r.overlaps(130,1));
    assert(!r.overlaps(1,99));assert(r.overlaps(0,101));assert(!r.overlaps(100,0));
    std::vector<std::thread> workers;
    for(unsigned t=0;t<8;++t)workers.emplace_back([&,t]{for(unsigned i=0;i<200;++i)r.include(1000+32*(i*8+t),16);});
    for(auto &t:workers)t.join();
    for(unsigned i=0;i<1600;++i){assert(r.overlaps(1000+32*i,16));assert(!r.overlaps(1016+32*i,16));}
    mng::port::ConservativeRanges overflow;overflow.include(UINT64_MAX-2,4);assert(overflow.overlaps(1,1));
    using Match=mng::port::ConservativeRanges::Match;
    assert(overflow.match(1,1)==Match::Invalid);
    mng::port::ConservativeRanges bounded(4);for(unsigned i=0;i<5;++i)bounded.include(2*i,1);
    assert(bounded.overlaps(100000,1));
    assert(bounded.match(100000,1)==Match::Exhausted);
    // A title with thousands of disjoint texture allocations must retain proof
    // of unrelated upload ranges rather than immediately losing all precision.
    mng::port::ConservativeRanges many;
    for(unsigned i=0;i<10000;++i)many.include(32*i,16);
    assert(many.match(20000,1)==Match::Overlap);
    assert(many.match(20016,1)==Match::Clear);
}
