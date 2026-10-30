// SPDX-License-Identifier: MIT
// Translation owns the engine lock. Only proven non-overlaps are cached;
// every new pending GPU write invalidates them before it becomes observable.
namespace mng_read_cache {
uint64_t generation=1, hits=0, misses=0;
struct Entry {uint64_t address{},bytes{},generation{};bool reports{};};
std::array<Entry,256> entries{};
bool enabled() {
    static const bool value=[] {const char *v=getenv("MNG_READ_RANGE_CACHE");return !v || strcmp(v,"0");}();
    return value;
}
Entry &entry(uint64_t address,uint64_t bytes,bool reports) {
    const uint64_t key=(address>>4)^(address>>17)^(bytes*0x9e3779b97f4a7c15ull)^uint64_t(reports);
    return entries[key&(entries.size()-1)];
}
bool contains(uint64_t address,uint64_t bytes,bool reports) {
    if(!enabled())return false;
    const auto &e=entry(address,bytes,reports);
    if(e.generation==generation && e.address==address && e.bytes==bytes && e.reports==reports){++hits;return true;}
    ++misses;return false;
}
void remember(uint64_t address,uint64_t bytes,bool reports) {
    if(enabled())entry(address,bytes,reports)={address,bytes,generation,reports};
}
void invalidate() {if(!++generation){entries={};generation=1;}}
}
