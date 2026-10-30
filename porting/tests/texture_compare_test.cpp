// SPDX-License-Identifier: MIT
#include "../runtime/texture_compare.h"
#include <cassert>
#include <array>
struct Value {void* mem{};std::array<unsigned,8> d{};unsigned has_layout{};unsigned char layout[8]{};uint64_t serial{};};
struct Object {uint64_t cookie{};Value value{};};
int main(){
    unsigned reads{};auto read=[&](const Object* p){++reads;return p->value;};
    auto equal=[&](const Object& a,const Object& b){return mng::port::texture_equal(&a,&b,read,0xfff);};
    Object a{},b{};assert(!equal(a,b) && !reads);
    a.cookie=1;assert(!equal(a,b) && !reads);b.cookie=2;assert(equal(a,b));
    a.value.mem=&a;b.value=a.value;assert(equal(a,b));
    b.value.mem=&b;assert(!equal(a,b));b.value=a.value;
    for(unsigned i=0;i<8;++i){b.value.d[i]=1;assert(!equal(a,b));b.value=a.value;}
    b.value.has_layout=1;assert(!equal(a,b));b.value=a.value;
    for(unsigned i=0;i<8;++i){b.value.layout[i]=1;assert(!equal(a,b));b.value=a.value;}
    // Immutable generations are deliberately excluded, swizzles are not.
    b.value.serial=0xffff000;assert(equal(a,b));b.value.serial|=1;assert(!equal(a,b));
    a.cookie=0;reads=0;assert(!equal(a,b) && !reads);
}
