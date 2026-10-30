// SPDX-License-Identifier: MIT
#include "keyed_jobs.h"
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <future>
#include <mutex>
#include <stdexcept>
#include <chrono>
using namespace std::chrono_literals;
int main(){
    std::mutex mu;std::condition_variable cv;unsigned entered=0;bool release=false;
    auto blocked=[&]{std::unique_lock l(mu);++entered;cv.notify_all();cv.wait(l,[&]{return release;});return true;};
    {
        mng::jobs::KeyedJobs jobs(2,4,1024);
        assert(jobs.enqueue("a",20,blocked));assert(jobs.enqueue("b",20,blocked));
        {std::unique_lock l(mu);assert(cv.wait_for(l,5s,[&]{return entered==2;}));}
        assert(jobs.enqueue("a",20,[]{assert(false);return false;}));
        assert(jobs.enqueue("c",20,[]{return true;}));
        assert(jobs.acquire("c")); // queued task claimed without waiting behind a/b
        auto waiting=std::async(std::launch::async,[&]{return jobs.acquire("a");});
        assert(waiting.wait_for(30ms)==std::future_status::timeout);
        assert(jobs.enqueue("bad",20,[]{throw std::runtime_error("expected");return false;}));
        assert(!jobs.enqueue("overflow",20,[]{return true;}));
        assert(!jobs.acquire("unknown"));
        {std::lock_guard l(mu);release=true;}cv.notify_all();
        assert(waiting.get());assert(jobs.acquire("b"));assert(!jobs.acquire("bad"));
        jobs.stop();auto s=jobs.stats();
        assert(s.built==3 && s.failed==1 && s.deduplicated==1 && s.claimed>=1 && s.waits>=1 && s.peak>=2);
        assert(!jobs.enqueue("late",0,[]{return true;}));
        printf("keyed jobs: built=%llu failed=%llu peak=%llu dedup=%llu claimed=%llu waits=%llu\n",(unsigned long long)s.built,(unsigned long long)s.failed,(unsigned long long)s.peak,(unsigned long long)s.deduplicated,(unsigned long long)s.claimed,(unsigned long long)s.waits);
    }
    // Shutdown must finish a running callback but cancel queued input owners.
    entered=0;release=false;
    {
        mng::jobs::KeyedJobs jobs(1,3,64);
        assert(!jobs.enqueue("oversize",65,[]{return true;}));
        assert(jobs.enqueue("a",20,blocked));
        {std::unique_lock l(mu);assert(cv.wait_for(l,5s,[&]{return entered==1;}));}
        assert(jobs.enqueue("b",20,[]{assert(false);return false;}));
        auto ending=std::async(std::launch::async,[&]{jobs.stop();});
        assert(ending.wait_for(30ms)==std::future_status::timeout);
        {std::lock_guard l(mu);release=true;}cv.notify_all();ending.get();
        auto s=jobs.stats();assert(s.built==1 && s.cancelled==1 && !s.active && s.rejected==1);
    }
    puts("keyed jobs: concurrency, dedup, acquire, failure, budget and shutdown passed");
}
