// SPDX-License-Identifier: MIT
#include "../runtime/serial_handoff.h"
#include <atomic>
#include <cassert>
#include <future>
#include <cstdio>
int main() {
    std::recursive_mutex engine;
    std::promise<void> release;
    auto gate=release.get_future().share();
    std::atomic<unsigned> completed{};
    {
        mng::port::SerialHandoff handoff(engine);
        auto first=handoff.submit([&]{gate.wait();assert(completed.fetch_add(1)==0);});
        assert(first==1);
        assert(!engine.try_lock()); // return guarantees ownership, not completion
        auto next=std::async(std::launch::async,[&]{
            handoff.submit([&]{assert(completed.fetch_add(1)==1);});
        });
        assert(next.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout);
        release.set_value();next.get();
        handoff.wait(2);
        std::promise<void> third_release;auto third_gate=third_release.get_future().share();
        auto third=handoff.submit([&]{third_gate.wait();++completed;});
        assert(third==3);
        // Reusing an older image must not wait for another image's present.
        auto older=std::async(std::launch::async,[&]{handoff.wait(first);});
        assert(older.wait_for(std::chrono::seconds(1))==std::future_status::ready);
        auto current=std::async(std::launch::async,[&]{handoff.wait(third);});
        assert(current.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout);
        third_release.set_value();current.get();
        // Destruction drains the last job before its captured state disappears.
    }
    assert(completed.load()==3 && engine.try_lock());engine.unlock();
    {
        mng::port::SerialHandoff handoff(engine);
        std::promise<void> waiting,release_wait;
        auto gate=release_wait.get_future().share();
        auto pin=std::make_shared<int>(42);std::weak_ptr<int> weak=pin;
        std::atomic<bool> retired{};
        auto ticket=handoff.submit([pin]{assert(*pin==42);},
            [&]{waiting.set_value();gate.wait();},[&]{retired=true;});
        pin.reset();waiting.get_future().get();
        assert(engine.try_lock());engine.unlock();
        assert(!weak.expired() && !retired); // ownership survives unlocked wait
        auto done=std::async(std::launch::async,[&]{handoff.wait(ticket);});
        assert(done.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout);
        auto next=std::async(std::launch::async,[&]{handoff.submit([&]{assert(retired);});});
        assert(next.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout);
        release_wait.set_value();done.get();next.get();handoff.wait(ticket+1);
        assert(retired && weak.expired());
    }
    puts("PASS: ordered prepare/retire, unlocked completion wait, bounded admission, retained resources, per-image tickets, shutdown drain");
}
