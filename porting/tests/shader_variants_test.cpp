// SPDX-License-Identifier: MIT
#include "../nvn_vulkan/shader_variants.h"
#include <cassert>
int main() {
    std::vector<uint8_t> bytes;
    auto word=[&](uint32_t n){for(unsigned i=0;i<4;++i)bytes.push_back(n>>(8*i));};
    word(0x31564758);word(1);word(0);word(2);
    for(int i=0;i<2;++i){std::string key(16,char('a'+i));bytes.insert(bytes.end(),key.begin(),key.end());word(1);word(0);word(6);word(i?2:0);}
    mng::shader::Family family;assert(mng::shader::parse(bytes,family));
    auto select=[&](int value){return mng::shader::select(family,[&](const auto&){return value;});};
    assert(select(0)->key==std::string(16,'a'));assert(select(2)->key==std::string(16,'b'));
    assert(!select(1));assert(select(-1));
    auto bad=bytes;bad.pop_back();assert(!mng::shader::parse(bad,family));
    bad=bytes;bad[16]='/';assert(!mng::shader::parse(bad,family));
    bad=bytes;bad[44]=3;assert(!mng::shader::parse(bad,family));
    bad=bytes;bad.push_back(0);assert(!mng::shader::parse(bad,family));
    bad=bytes;bad[40]=16;assert(!mng::shader::parse(bad,family));
    // Fragment outputs never inherit the unbound-attribute wildcard.
    for(auto& row:family.variants)for(auto& c:row.conditions)c.kind=1;
    assert(!select(-1));
}
