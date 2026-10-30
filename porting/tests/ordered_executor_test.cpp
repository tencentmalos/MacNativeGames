// SPDX-License-Identifier: MIT
#include "../runtime/ordered_executor.h"
#include "../runtime/submission_inputs.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <future>
#include <vector>
using namespace mng::port;
using namespace std::chrono_literals;
int main(){
    {
        OrderedExecutor q(2,16);std::promise<void> gate;auto ready=gate.get_future().share();
        std::atomic<bool> running=false,third=false;
        auto a=q.submit([&]{running=true;ready.wait();return 17;},8);
        while(!running)std::this_thread::yield();
        auto b=q.submit([]{return 29;},8);
        assert(q.tail().sequence()==b.sequence());
        assert(!a.wait(0) && !b.wait(1000000));
        auto producer=std::async(std::launch::async,[&]{auto t=q.submit([]{return 31;},1);third=true;return t;});
        assert(producer.wait_for(10ms)==std::future_status::timeout && !third);
        assert(q.stats().bytes==16 && q.stats().pending==2);
        gate.set_value();auto c=producer.get();q.drain();
        assert(a.value()==17 && b.value()==29 && c.value()==31);
        assert(a.sequence()==1 && b.sequence()==2 && c.sequence()==3);
        assert(q.stats().bytes==0 && q.stats().peak_bytes==16 && q.stats().peak_pending==2);
        q.close();bool refused=false;try{q.submit([]{return 0;});}catch(const std::runtime_error&){refused=true;}assert(refused);
    }
    {
        OrderedExecutor q(4,1024);std::vector<unsigned> observed;std::mutex publish;
        unsigned sequence=0;std::vector<std::thread> producers;
        for(unsigned i=0;i<6;++i)producers.emplace_back([&]{for(unsigned j=0;j<1000;++j){
            std::lock_guard lock(publish);const auto n=++sequence;q.submit([&,n]{observed.push_back(n);return n;},64);
        }});
        for(auto &p:producers)p.join();q.drain();
        assert(observed.size()==6000);for(unsigned i=0;i<observed.size();++i)assert(observed[i]==i+1);
        assert(q.stats().peak_pending<=4 && q.stats().peak_bytes<=256);
    }
    {
        OrderedExecutor q;std::promise<void> gate;auto ready=gate.get_future().share();std::atomic<unsigned> ran=0;
        auto a=q.submit([&]()->uint64_t{ready.wait();throw std::runtime_error("injected");});
        auto b=q.submit([&]{++ran;return 0;});gate.set_value();
        for(auto t:{a,b}){bool failed=false;try{t.wait();}catch(const std::runtime_error&){failed=true;}assert(failed);}
        assert(!ran);q.close();assert(q.stats().pending==0);
    }
    {
        OrderedExecutor::Ticket retained;std::weak_ptr<int> weak;
        {OrderedExecutor q;auto owned=std::make_shared<int>(42);weak=owned;
         q.submit([owned]{return uint64_t(*owned);});
         retained=q.tail();assert(retained.sequence()==1);
         q.submit([]{return uint64_t(43);});assert(q.tail().sequence()==2);owned.reset();}
        assert(retained.value()==42 && weak.expired());
    }
    {
        std::array<uint8_t,256> guest{};for(unsigned i=0;i<guest.size();++i)guest[i]=uint8_t(i);
        const auto base=uint64_t(guest.data());SubmissionInputs in;
        for(unsigned i=0;i<100000;++i)assert(in.add(base+32,64));
        assert(in.add(base,64) && in.add(base+96,160));
        assert(!in.find(base,1));assert(in.capture([&](auto a,auto n){return a==base && n==256;}));
        assert(in.ranges().size()==1 && in.bytes()==256);guest.fill(0);
        for(unsigned i=0;i<256;++i)assert(*static_cast<const uint8_t*>(in.find(base+i,1))==uint8_t(i));
        assert(!in.find(base-1,2) && !in.find(base+255,2) && !in.find(UINT64_MAX,2));
        assert(!in.add(base,1) && !in.capture([](auto,auto){return true;}));
        SubmissionInputs bad;assert(!bad.add(UINT64_MAX,2));
        SubmissionInputs huge;assert(!huge.add(base,SubmissionInputs::limit+1));
        SubmissionInputs hazard;assert(hazard.add(base,1));assert(!hazard.capture([](auto,auto){return false;}));assert(!hazard.find(base,1));
    }
    {
        alignas(16) std::array<uint8_t,64> memory{};const auto base=uint64_t(memory.data());
        auto pool=std::make_shared<ReportPool>(base,memory.size(),1);
        assert(pool->contains(base,16) && pool->contains(base+48,16));
        assert(!pool->contains(base-1,16) && !pool->contains(base+49,16));
        assert(!ReportPool(UINT64_MAX-8,16,2).contains(UINT64_MAX-8,16));
        SubmissionInputs input;
        assert(!input.add_report(base+1,pool) && !input.add_report(base+64,pool) && !input.add_report(base,{}));
        assert(input.add_report(base,pool) && input.add(base+16,16) && input.reports_disjoint());
        assert(input.capture([](auto,auto){return true;}));
        auto retained=input.report_pool(base);assert(retained==pool && !input.report_pool(base+16));
        assert(!input.add_report(base,pool));
        assert(input.retained_bytes()>=16+sizeof(SubmissionInputs::Range)+sizeof(SubmissionInputs::Report));
        SubmissionInputs alias;assert(alias.add_report(base,pool) && alias.add(base+15,1));
        assert(!alias.reports_disjoint() && !alias.capture([](auto,auto){return true;}));
        SubmissionInputs bounded;
        for(size_t i=0;i<SubmissionInputs::report_limit;++i)assert(bounded.add_report(base,pool));
        assert(!bounded.add_report(base,pool));
        pool->alive=false;assert(!retained->contains(base,16) && !input.report_pool(base));
        auto replacement=std::make_shared<ReportPool>(base,memory.size(),2);
        assert(replacement->contains(base,16) && !retained->contains(base,16));
    }
    {
        WaitBudget finite(1000000);std::this_thread::sleep_for(2ms);assert(!finite.remaining());
        assert(!WaitBudget(0).remaining());assert(WaitBudget(UINT64_MAX).remaining()==UINT64_MAX);
    }
    puts("Ordered execution: FIFO, budgets, backpressure, poll/deadline, failure, shutdown and immutable inputs passed");
}
