// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace mng::port {
// A renderer ownership thread, not a general worker pool. Admission is bounded
// through retirement (including the running closure). The caller serializes
// publication with frontend mutations; the worker never takes that lock.
class OrderedExecutor {
public:
    class Ticket {
        friend class OrderedExecutor;
        struct State {
            std::mutex mutex;
            std::condition_variable cv;
            bool done{};
            uint64_t value{};
            std::exception_ptr error;
        };
        std::shared_ptr<State> state_;
        uint64_t sequence_{};
        Ticket(uint64_t sequence,std::shared_ptr<State> state)
            :state_(std::move(state)),sequence_(sequence) {}
    public:
        Ticket()=default;
        uint64_t sequence()const{return sequence_;}
        bool wait(uint64_t timeout=UINT64_MAX)const {
            if(!state_)return true;
            std::unique_lock lock(state_->mutex);
            if(!state_->done){
                if(!timeout)return false;
                if(timeout==UINT64_MAX)state_->cv.wait(lock,[&]{return state_->done;});
                else if(!state_->cv.wait_for(lock,std::chrono::nanoseconds(
                    std::min<uint64_t>(timeout,INT64_MAX)),[&]{return state_->done;}))return false;
            }
            if(state_->error)std::rethrow_exception(state_->error);
            return true;
        }
        uint64_t value()const {
            wait();if(!state_)return 0;
            std::lock_guard lock(state_->mutex);return state_->value;
        }
        explicit operator bool()const{return bool(state_);}
    };
    struct Stats {uint64_t admitted{},completed{},bytes{},peak_bytes{},pending{},peak_pending{};};
    explicit OrderedExecutor(size_t count=64,size_t bytes=64u<<20)
        :count_limit_(count),byte_limit_(bytes) {
        if(!count || !bytes)throw std::invalid_argument("Empty ordered execution budget");
        worker_=std::thread([this]{run();});
    }
    ~OrderedExecutor(){close();}
    OrderedExecutor(const OrderedExecutor&)=delete;
    OrderedExecutor& operator=(const OrderedExecutor&)=delete;
    Ticket submit(std::function<uint64_t()> job,size_t bytes=0) {
        if(!job || bytes>byte_limit_)throw std::length_error("Ordered request exceeds budget");
        std::unique_lock lock(mutex_);
        cv_.wait(lock,[&]{return stop_||error_||(stats_.pending<count_limit_ && bytes<=byte_limit_-stats_.bytes);});
        if(error_)std::rethrow_exception(error_);
        if(stop_)throw std::runtime_error("Ordered executor stopped");
        if(stats_.admitted==UINT64_MAX)throw std::overflow_error("Ordered sequence exhausted");
        auto state=std::make_shared<Ticket::State>();
        const auto sequence=stats_.admitted+1;
        queue_.push_back({sequence,bytes,std::move(job),state});
        ++stats_.admitted;++stats_.pending;stats_.bytes+=bytes;
        stats_.peak_bytes=std::max(stats_.peak_bytes,stats_.bytes);
        stats_.peak_pending=std::max(stats_.peak_pending,stats_.pending);
        last_=Ticket(sequence,std::move(state));cv_.notify_all();return last_;
    }
    // Publication must be excluded by the caller until its protected mutation
    // finishes. A drain is not a lock and does not exclude later producers.
    Ticket tail()const {std::lock_guard lock(mutex_);return last_;}
    void drain()const {tail().wait();}
    Stats stats()const {std::lock_guard lock(mutex_);return stats_;}
    void close() noexcept {
        {std::lock_guard lock(mutex_);stop_=true;}cv_.notify_all();
        if(worker_.joinable())worker_.join();
    }
private:
    struct Request {uint64_t sequence;size_t bytes;std::function<uint64_t()> job;std::shared_ptr<Ticket::State> state;};
    void run() noexcept {
        for(;;){
            Request request;std::exception_ptr error;
            {std::unique_lock lock(mutex_);cv_.wait(lock,[&]{return stop_||!queue_.empty();});
             if(queue_.empty())return;
             request=std::move(queue_.front());queue_.pop_front();error=error_;}
            uint64_t value{};
            if(!error)try{value=request.job();}catch(...){error=std::current_exception();}
            // Release every captured input before publishing CPU completion.
            request.job={};
            {std::lock_guard lock(mutex_);if(error && !error_)error_=error;
             stats_.completed=request.sequence;--stats_.pending;stats_.bytes-=request.bytes;}
            {std::lock_guard lock(request.state->mutex);request.state->value=value;
             request.state->error=error;request.state->done=true;}
            request.state->cv.notify_all();cv_.notify_all();
        }
    }
    const size_t count_limit_,byte_limit_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> queue_;
    Ticket last_;
    Stats stats_;
    std::exception_ptr error_;
    bool stop_{};
    std::thread worker_;
};

class WaitBudget {
public:
    explicit WaitBudget(uint64_t ns):start_(std::chrono::steady_clock::now()),ns_(ns){}
    uint64_t remaining()const {
        if(ns_==UINT64_MAX)return ns_;
        const auto elapsed=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-start_).count());
        return elapsed>=ns_?0:ns_-elapsed;
    }
private:
    std::chrono::steady_clock::time_point start_;
    uint64_t ns_;
};
}
