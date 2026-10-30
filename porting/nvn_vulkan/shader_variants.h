// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mng::shader {
// XGV1: little-endian header {magic, version, stage, rows}; each row has a
// content-hash key (16 ASCII hex bytes), condition count, then {kind, slot, class}.
// Classes: float=0, sint=1, uint=2. Kinds: vertex attribute=0, fragment output=1.
struct Condition { uint32_t kind, slot, scalar; };
struct Variant { std::string key; std::vector<Condition> conditions; };
struct Family { uint32_t stage{}; std::vector<Variant> variants; };
inline bool parse(const std::vector<uint8_t>& bytes, Family& result) {
    if(bytes.size()<16 || bytes.size()>65536) return false;
    size_t pos=0;
    auto word=[&](){uint32_t value=0;for(unsigned i=0;i<4;++i)value|=uint32_t(bytes[pos++])<<(i*8);return value;};
    if(word()!=0x31564758 || word()!=1) return false;
    Family out;out.stage=word();uint32_t count=word();
    if((out.stage!=0 && out.stage!=4) || !count || count>64) return false;
    for(uint32_t i=0;i<count;++i){
        if(bytes.size()-pos<20) return false;
        Variant row;row.key.assign(reinterpret_cast<const char*>(bytes.data()+pos),16);pos+=16;
        for(char c:row.key)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
        uint32_t n=word();if(n>24 || bytes.size()-pos<size_t(n)*12)return false;
        uint32_t seen=0;
        for(uint32_t j=0;j<n;++j){
            Condition c{word(),word(),word()};
            if(c.kind>1 || c.scalar>2 || c.slot>=(c.kind?8u:16u) || c.kind!=(out.stage==4))return false;
            if(seen&(1u<<c.slot))return false;seen|=1u<<c.slot;row.conditions.push_back(c);
        }
        out.variants.push_back(std::move(row));
    }
    if(pos!=bytes.size())return false;result=std::move(out);return true;
}
// Answer -1 means an unbound vertex attribute: the renderer supplies typed zero.
template<class Answer> inline const Variant* select(const Family& family, Answer answer) {
    for(const auto& row:family.variants){
        bool match=true;
        for(const auto& c:row.conditions){int value=answer(c);if(value<0 && c.kind==0)continue;if(value!=int(c.scalar)){match=false;break;}}
        if(match)return &row;
    }
    return nullptr;
}
} // namespace mng::shader
