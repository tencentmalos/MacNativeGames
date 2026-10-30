// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <algorithm>

namespace mng::port {
// API pointers must refer to static names. No callback or renderer lock is
// entered under this mutex. A reset invalidates in-flight diagnostic samples.
class FrontLockStats {
public:
    struct Row {
        const char *waiter{}, *holder{};
        uint32_t handle{};
        uint64_t count{}, total_ns{}, max_ns{};
    };
    struct Snapshot {
        uint64_t epoch{}, other_count{}, other_ns{}, other_max_ns{};
        unsigned size{};
        std::array<Row,128> rows{};
    };
    uint64_t token() const {
        const auto e=epoch_.load(std::memory_order_relaxed);return e&1?e:0;
    }
    void record(uint64_t token,const char* waiter,uint32_t handle,const char* holder,uint64_t ns) {
        if(!token)return;
        std::lock_guard lock(mutex_);
        if(token!=epoch_.load(std::memory_order_relaxed))return;
        for(unsigned i=0;i<data_.size;++i){auto &r=data_.rows[i];
            if(r.waiter==waiter && r.holder==holder && r.handle==handle){
                ++r.count;r.total_ns+=ns;r.max_ns=std::max(r.max_ns,ns);return;}}
        if(data_.size<data_.rows.size())data_.rows[data_.size++]={waiter,holder,handle,1,ns,ns};
        else{++data_.other_count;data_.other_ns+=ns;data_.other_max_ns=std::max(data_.other_max_ns,ns);}
    }
    // -1=status, 0=off, 1=on, 2=reset. Never block a debugbus service.
    bool control(int action,Snapshot& out) {
        std::unique_lock lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return false;
        auto e=epoch_.load(std::memory_order_relaxed);
        if(action==2){data_={};e+=2;}
        else if(action>=0 && bool(e&1)!=bool(action))++e;
        epoch_.store(e,std::memory_order_relaxed);out=data_;out.epoch=e;return true;
    }
private:
    std::atomic<uint64_t> epoch_{};
    std::mutex mutex_;
    Snapshot data_;
};
}
