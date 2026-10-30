// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <atomic>
#include <memory>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>

namespace mng::port {
enum class InputFailure : uint32_t { None, ShaderVariant, Binding, WritableStorage, RangeBudget, Alias, Other };
struct SubmissionSite {
    uint32_t opcode=UINT32_MAX,handle=UINT32_MAX;
    uint64_t offset{};
    InputFailure input_failure{};
    uint32_t stage=UINT32_MAX,binding=UINT32_MAX;
    uint64_t requested_bytes{},accumulated_bytes{};
};
// The caller owns storage. Finalize must drain translation and retire GPU writes
// before invalidating this generation or releasing/remapping its backing memory.
struct ReportPool {
    const uint64_t begin,bytes,generation;
    std::atomic<bool> alive{true};
    ReportPool(uint64_t address,uint64_t size,uint64_t serial):begin(address),bytes(size),generation(serial){}
    bool contains(uint64_t address,uint64_t size)const {
        return alive.load(std::memory_order_acquire) && generation && begin &&
            bytes<=UINT64_MAX-begin && address>=begin && address-begin<=bytes && size<=bytes-(address-begin);
    }
};
// Submit-visible bytes keyed by original addresses. GPU addresses are never
// rewritten, and writable/GPU-produced inputs must be rejected by the caller.
class SubmissionInputs {
public:
    struct Range {uint64_t begin,end;size_t offset{};};
    static constexpr size_t limit=32u<<20;
    static constexpr size_t report_bytes=16,report_limit=4096;
    struct Report {uint64_t address;std::shared_ptr<const ReportPool> pool;};
    bool add_report(uint64_t address,std::shared_ptr<const ReportPool> pool){
        if(captured_ || (address&15) || !pool || !pool->contains(address,report_bytes) || reports_.size()>=report_limit)return false;
        reports_.push_back({address,std::move(pool)});return true;
    }
    const std::vector<Report>& reports()const{return reports_;}
    std::shared_ptr<const ReportPool> report_pool(uint64_t address)const {
        for(const auto &r:reports_)if(r.address==address && r.pool->contains(address,report_bytes))return r.pool;
        return {};
    }
    bool reports_disjoint()const {
        if(captured_)return false;
        for(const auto &r:reports_)if(!r.pool->contains(r.address,report_bytes) || overlaps(r.address,report_bytes))return false;
        return true;
    }
    bool add(uint64_t address,uint64_t bytes) {
        if(captured_)return false;
        if(!bytes)return true;
        if(!address || bytes>UINT64_MAX-address)return false;
        auto it=pending_.lower_bound(address);
        if(it!=pending_.begin()){
            auto prev=std::prev(it);
            if(prev->second>=address)it=prev;
        }
        uint64_t end=address+bytes;
        while(it!=pending_.end() && it->first<=end){
            address=std::min(address,it->first);end=std::max(end,it->second);
            total_-=size_t(it->second-it->first);it=pending_.erase(it);
        }
        if(end-address>limit-total_ || pending_.size()>=65536)return false;
        total_+=size_t(end-address);pending_.emplace(address,end);return true;
    }
    template<class Safe> bool capture(Safe safe) {
        if(captured_ || !reports_disjoint())return false;
        ranges_.clear();size_t offset=0;
        for(const auto &[begin,end]:pending_){
            if(!safe(begin,end-begin)){ranges_.clear();return false;}
            ranges_.push_back({begin,end,offset});offset+=size_t(end-begin);
        }
        bytes_.resize(offset);
        for(const auto &r:ranges_)std::memcpy(bytes_.data()+r.offset,
            reinterpret_cast<const void*>(r.begin),size_t(r.end-r.begin));
        pending_.clear();captured_=true;return true;
    }
    const void *find(uint64_t address,size_t bytes)const {
        if(!captured_ || !bytes || bytes>UINT64_MAX-address)return nullptr;
        auto it=std::upper_bound(ranges_.begin(),ranges_.end(),address,
            [](uint64_t a,const Range &r){return a<r.begin;});
        if(it==ranges_.begin())return nullptr;--it;
        if(address+bytes>it->end)return nullptr;
        return bytes_.data()+it->offset+size_t(address-it->begin);
    }
    bool overlaps(uint64_t address,uint64_t bytes)const {
        if(!bytes)return false;
        if(bytes>UINT64_MAX-address)return true;
        auto it=pending_.lower_bound(address);
        if(it!=pending_.end() && it->first<address+bytes)return true;
        return it!=pending_.begin() && std::prev(it)->second>address;
    }
    size_t pending_bytes()const{return total_;}
    size_t bytes()const{return bytes_.size();}
    size_t retained_bytes()const{return bytes_.capacity()+ranges_.capacity()*sizeof(Range)+reports_.capacity()*sizeof(Report);}
    const std::vector<Range>& ranges()const{return ranges_;}
private:
    std::vector<Report> reports_;
    std::map<uint64_t,uint64_t> pending_;
    std::vector<Range> ranges_;
    std::vector<uint8_t> bytes_;
    size_t total_{};
    bool captured_{};
};
}
