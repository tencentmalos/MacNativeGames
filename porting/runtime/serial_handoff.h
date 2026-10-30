// SPDX-License-Identifier: MIT
#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace mng::port {
// One bounded job with prepare/retire under the engine lock and a completion
// wait outside it. submit() returns only after the worker owns the lock;
// subsequent engine calls cannot overtake prepare. The ticket completes after
// retirement, not when the GPU wait starts. Captures stay alive through retire.
// Call submit/destruction without holding the engine lock.
class SerialHandoff {
public:
    explicit SerialHandoff(std::recursive_mutex& engine) : engine_(engine), worker_([this]{run();}) {}
    ~SerialHandoff() {
        {std::lock_guard lock(mutex_);stop_=true;}
        cv_.notify_all();worker_.join();
    }
    SerialHandoff(const SerialHandoff&)=delete;
    SerialHandoff& operator=(const SerialHandoff&)=delete;
    uint64_t submit(std::function<void()> job, std::function<void()> wait={},
                    std::function<void()> retire={}) {
        std::unique_lock lock(mutex_);
        cv_.wait(lock,[&]{return !busy_;});
        const auto ticket=++submitted_;busy_=true;job_=std::move(job);
        wait_=std::move(wait);retire_=std::move(retire);cv_.notify_all();
        cv_.wait(lock,[&]{return started_>=ticket;});
        return ticket;
    }
    void wait(uint64_t ticket) {
        std::unique_lock lock(mutex_);
        cv_.wait(lock,[&]{return completed_>=ticket;});
    }
private:
    void run() {
        for(;;) {
            std::function<void()> job,wait,retire;uint64_t ticket;
            {std::unique_lock lock(mutex_);cv_.wait(lock,[&]{return stop_||bool(job_);});
             if(!job_)return;job=std::exchange(job_,{});wait=std::exchange(wait_,{});
             retire=std::exchange(retire_,{});ticket=submitted_;}
            {
                std::lock_guard engine(engine_);
                {std::lock_guard lock(mutex_);started_=ticket;cv_.notify_all();}
                job();
            }
            if(wait)wait();
            {std::lock_guard engine(engine_);if(retire)retire();
             // Resource-owning closure destructors can touch renderer state.
             job={};wait={};retire={};}
            {std::lock_guard lock(mutex_);completed_=ticket;busy_=false;cv_.notify_all();}
        }
    }
    std::recursive_mutex& engine_;
    std::mutex mutex_;std::condition_variable cv_;
    std::function<void()> job_,wait_,retire_;
    uint64_t submitted_{},started_{},completed_{};bool busy_{},stop_{};
    std::thread worker_;
};
}
