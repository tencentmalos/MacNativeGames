// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>

namespace mng::port {
// One externally serialized consumer; producers never enter the engine.
// The queue mutex must not be followed by any other lock. Commit is restricted
// to noexcept inline Guest-memory writes and atomic counters, never callbacks
// into the renderer. Two fixed buffers avoid allocating on publication/drain.
template<class T,size_t Capacity> class PublicationQueue {
    static_assert(Capacity && std::is_nothrow_copy_assignable_v<T>);
public:
    struct Stats {
        bool enabled{},failed{};
        uint64_t enqueued{},drained{},overflow{},max_depth{},pending{},in_flight{};
        uint64_t drain_ns_total{},drain_ns_max{},drain_calls{};
    };
    bool enabled()const{return enabled_.load(std::memory_order_acquire);}
    bool pending()const{return pending_.load(std::memory_order_acquire)!=0 || poisoned_.load(std::memory_order_acquire);}
    template<class Commit> bool publish(const T* operation,Commit commit,uint64_t* sequence=nullptr){
        static_assert(std::is_nothrow_invocable_v<Commit>);
        std::lock_guard lock(mutex_);check();
        if(!stats_.enabled)return false;
        if(operation && stats_.pending==Capacity){++stats_.overflow;return false;}
        if(operation){
            slots_[stats_.pending++]=*operation;++stats_.enqueued;
            stats_.max_depth=std::max(stats_.max_depth,stats_.pending);
            if(sequence)*sequence=stats_.enqueued;
        }
        commit();pending_.store(stats_.pending,std::memory_order_release);return true;
    }
    bool publish(const T& operation,uint64_t* sequence=nullptr){
        return publish(&operation,[]()noexcept{},sequence);
    }
    // The caller holds its consumer/front lock. Disable admission before drain
    // so an in-flight producer either committed before this cut or falls back.
    bool try_enable(bool enabled){
        std::unique_lock lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return false;
        check();stats_.enabled=enabled;enabled_.store(enabled,std::memory_order_release);return true;
    }
    bool snapshot(Stats& out){
        std::unique_lock lock(mutex_,std::try_to_lock);if(!lock.owns_lock())return false;
        out=stats_;return true;
    }
    void reset(bool enabled){
        std::lock_guard lock(mutex_);check();
        if(stats_.pending || stats_.in_flight)throw std::logic_error("Reset with pending publications");
        stats_={};stats_.enabled=enabled;enabled_.store(enabled,std::memory_order_release);
    }
    template<class Execute> void drain(Execute execute){
        if(!pending())return;
        const auto begin=std::chrono::steady_clock::now();uint64_t count{};
        {
            std::lock_guard lock(mutex_);check();
            if(stats_.in_flight)throw std::logic_error("Concurrent publication consumers");
            if(!stats_.pending)return;
            slots_.swap(batch_);count=stats_.pending;stats_.pending=0;
            stats_.in_flight=count;pending_.store(0,std::memory_order_release);
        }
        uint64_t done{};
        try{for(;done<count;++done)execute(batch_[done]);}
        catch(...){
            // Keep the failed operation and remainder in batch_. Never resume
            // an ambiguous partially executed packet or silently lose it.
            std::lock_guard lock(mutex_);stats_.drained+=done;stats_.in_flight=count-done;
            stats_.failed=true;stats_.enabled=false;enabled_.store(false,std::memory_order_release);
            failure_=std::current_exception();poisoned_.store(true,std::memory_order_release);throw;
        }
        const auto ns=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count());
        std::lock_guard lock(mutex_);stats_.drained+=count;stats_.in_flight=0;
        ++stats_.drain_calls;stats_.drain_ns_total+=ns;stats_.drain_ns_max=std::max(stats_.drain_ns_max,ns);
    }
private:
    void check(){if(failure_)std::rethrow_exception(failure_);}
    std::mutex mutex_;
    std::unique_ptr<T[]> slots_{new T[Capacity]},batch_{new T[Capacity]};
    std::atomic<bool> enabled_{};
    std::atomic<bool> poisoned_{};
    std::atomic<uint64_t> pending_{};
    Stats stats_;
    std::exception_ptr failure_;
};
}
