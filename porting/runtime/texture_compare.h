// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstring>
namespace mng::port {
template<class Object,class Read> bool texture_equal(const Object* a,const Object* b,Read read,uint64_t swizzle_mask){
    uint64_t ka,kb;std::memcpy(&ka,a,8);std::memcpy(&kb,b,8);
    if(!ka || !kb)return false;
    const auto x=read(a),y=read(b);
    if(x.mem!=y.mem || std::memcmp(&x.d,&y.d,sizeof(x.d)) || x.has_layout!=y.has_layout ||
       std::memcmp(x.layout,y.layout,8))return false;
    return (x.serial&swizzle_mask)==(y.serial&swizzle_mask);
}
}
