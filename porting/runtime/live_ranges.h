// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

namespace mng::port {
// Publication precedes the operation that can produce CPU-visible output.
// Copies share one registration; only the last owner retires it. Queries never
// acquire a renderer/frontend lock. Unknown ranges fail closed until retired.
class LiveRanges {
public:
    enum Kind : unsigned { Image=1, Writeback=2, Report=4, Event=8, Unknown=16 };
private:
    struct Range {uint64_t address,bytes;unsigned kind;};
    struct State {
        std::mutex mutex;
        std::map<uint64_t,Range> entries;
        uint64_t next=0,unknown=0;
        const size_t capacity;
        explicit State(size_t n):capacity(n){}
    };
    struct Registration {
        std::shared_ptr<State> state;
        uint64_t id=0;
        bool unknown=false;
        ~Registration(){
            if(!state)return;
            std::lock_guard lock(state->mutex);
            if(unknown)--state->unknown;
            else if(id)state->entries.erase(id);
        }
    };
    std::shared_ptr<State> state_;
public:
    using Lease=std::shared_ptr<const Registration>;
    struct Stats {size_t ranges;uint64_t unknown;};
    explicit LiveRanges(size_t capacity=65536):state_(std::make_shared<State>(capacity)){}
    Lease retain(uint64_t address,uint64_t bytes,unsigned kind){
        if(!bytes)return {};
        auto token=std::make_shared<Registration>();token->state=state_;
        std::lock_guard lock(state_->mutex);
        if(!address || bytes>UINT64_MAX-address || !kind ||
           state_->entries.size()>=state_->capacity || state_->next==UINT64_MAX){
            ++state_->unknown;token->unknown=true;
        }else{
            const auto id=++state_->next;
            state_->entries.emplace(id,Range{address,bytes,kind});token->id=id;
        }
        return token;
    }
    unsigned match(uint64_t address,uint64_t bytes)const {
        if(!bytes)return 0;
        if(!address || bytes>UINT64_MAX-address)return Unknown;
        std::lock_guard lock(state_->mutex);
        unsigned kinds=state_->unknown?Unknown:0;
        for(const auto &[id,r]:state_->entries)
            if(address<r.address+r.bytes && r.address<address+bytes)kinds|=r.kind;
        return kinds;
    }
    Stats stats()const {
        std::lock_guard lock(state_->mutex);
        return {state_->entries.size(),state_->unknown};
    }
};
// The registry can outlive frontend teardown while deferred GPU owners retire.
// No reset invalidates an old generation or makes a live alias appear absent.
inline LiveRanges &flush_dependencies(){static auto *value=new LiveRanges;return *value;}
}
