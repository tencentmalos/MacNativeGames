// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <algorithm>
#include <iterator>
#include <map>
#include <mutex>
#include <limits>

namespace mng::port {
// Monotonic potential-write coverage. Entries never expire while a session is
// live, so retirement, serial aliases and address reuse cannot produce a false
// negative. Capacity exhaustion or invalid ranges fail closed.
class ConservativeRanges {
public:
    enum class Match { Clear, Overlap, Exhausted, Invalid };
    explicit ConservativeRanges(size_t capacity=65536) : capacity_(capacity) {}
    void include(uint64_t start,uint64_t bytes) {
        if(!bytes)return;
        std::lock_guard lock(mutex_);
        if(all_)return;
        if(bytes>std::numeric_limits<uint64_t>::max()-start){all_=true;invalid_=true;ranges_.clear();return;}
        uint64_t end=start+bytes;
        auto it=ranges_.lower_bound(start);
        if(it!=ranges_.begin()){auto previous=std::prev(it);if(previous->second>=start)it=previous;}
        while(it!=ranges_.end() && it->first<=end){
            if(it->second<start){++it;continue;}
            start=std::min(start,it->first);end=std::max(end,it->second);it=ranges_.erase(it);
        }
        if(ranges_.size()>=capacity_){all_=true;ranges_.clear();return;}
        ranges_.emplace(start,end);
    }
    bool overlaps(uint64_t start,uint64_t bytes) const {
        return match(start,bytes)!=Match::Clear;
    }
    Match match(uint64_t start,uint64_t bytes) const {
        if(!bytes)return Match::Clear;
        std::lock_guard lock(mutex_);
        if(invalid_ || bytes>std::numeric_limits<uint64_t>::max()-start)return Match::Invalid;
        if(all_)return Match::Exhausted;
        auto it=ranges_.lower_bound(start+bytes);
        return it!=ranges_.begin() && std::prev(it)->second>start ? Match::Overlap : Match::Clear;
    }
private:
    mutable std::mutex mutex_;
    std::map<uint64_t,uint64_t> ranges_;
    const size_t capacity_;
    bool all_=false,invalid_=false;
};
}
