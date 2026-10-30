// SPDX-License-Identifier: MIT
#include "../runtime/publication_queue.h"
#include <cassert>
#include <thread>
#include <vector>
#include <array>
struct Op {unsigned kind{},id{},pool{};uint64_t serial{};};
int main(){
    using Queue=mng::port::PublicationQueue<Op,2>;Queue queue;Queue::Stats stats;
    std::vector<Op> seen;auto execute=[&](const Op& op){seen.push_back(op);};
    Op retire{0,1,7,10},texture{1,2,7,11},sampler{2,3,7,12};
    bool committed=false;assert(!queue.publish(&retire,[&]()noexcept{committed=true;}));assert(!committed);
    assert(queue.try_enable(true));
    assert(queue.publish(&retire,[&]()noexcept{committed=true;}));assert(committed);
    assert(queue.publish(texture));assert(!queue.publish(sampler));
    // I1/I2: retire the old serial before registering a different identity at
    // the same descriptor slot. I5: pool finalization follows a complete drain.
    queue.drain(execute);execute(sampler);assert(seen.size()==3);
    assert(seen[0].serial==10 && seen[1].serial==11 && seen[2].kind==2);
    assert(queue.snapshot(stats) && stats.enqueued==2 && stats.drained==2 && stats.overflow==1);
    assert(stats.max_depth==2 && !stats.pending && !stats.in_flight && stats.drain_calls==1);
    assert(queue.publish(retire));assert(queue.try_enable(false));assert(!queue.publish(texture));
    queue.drain(execute);assert(queue.snapshot(stats) && !stats.pending && stats.drained==3);
    queue.reset(false);assert(queue.snapshot(stats) && !stats.enabled && !stats.enqueued);
    // I6: an ambiguous partially executed packet is poisoned and retained.
    Queue broken;assert(broken.try_enable(true));assert(broken.publish(retire));assert(broken.publish(texture));
    unsigned called=0;bool threw=false;
    try{broken.drain([&](const Op&){if(++called==2)throw std::runtime_error("injected");});}catch(const std::runtime_error&){threw=true;}
    assert(threw && broken.snapshot(stats) && stats.failed && stats.drained==1 && stats.in_flight==1);
    threw=false;try{broken.publish(retire);}catch(const std::runtime_error&){threw=true;}assert(threw);
    threw=false;try{broken.drain(execute);}catch(const std::runtime_error&){threw=true;}assert(threw);
    // I8: concurrent producers publish value copies in mutex-admission order.
    // The consumer drains while they run; sequence IDs are used only by tests.
    constexpr unsigned producers=8,each=10000,total=producers*each;
    mng::port::PublicationQueue<Op,128> concurrent;assert(concurrent.try_enable(true));
    std::vector<uint64_t> order(total);std::vector<unsigned> actual;actual.reserve(total);
    std::atomic<unsigned> finished{};
    std::thread consumer([&]{while(finished.load()!=producers || concurrent.pending()){
        concurrent.drain([&](const Op& op){actual.push_back(op.id);});std::this_thread::yield();}});
    std::vector<std::thread> workers;
    for(unsigned t=0;t<producers;++t)workers.emplace_back([&,t]{
        for(unsigned i=0;i<each;++i){const unsigned id=t*each+i;Op op{1,id,7,id+1};
            while(!concurrent.publish(op,&order[id]))std::this_thread::yield();
            op.id=~id; // Published entries must never refer to producer storage.
        }++finished;});
    for(auto& worker:workers)worker.join();consumer.join();assert(actual.size()==total);
    for(unsigned i=0;i<total;++i)assert(order[actual[i]]==i+1);
    mng::port::PublicationQueue<Op,128>::Stats final;assert(concurrent.snapshot(final));
    assert(final.enqueued==total && final.drained==total && !final.pending && !final.failed && final.max_depth<=128);
}
