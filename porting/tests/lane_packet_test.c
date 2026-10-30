// SPDX-License-Identifier: MIT
#include "../runtime/lane_packet.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>

int main(void) {
    const int rows[]={-2,-1,0,255},hle[]={INT_MIN,-1,0,INT_MAX},svc[]={-1,0,255};
    for(unsigned r=0;r<4;++r)for(int c=-1;c<3;++c)for(int k=1;k<=3;++k)
    for(unsigned h=0;h<4;++h)for(unsigned v=0;v<3;++v)for(unsigned mask=0;mask<8;++mask){
        mng_lane_event a={UINT64_MAX,mask,k,rows[r],c,svc[v],hle[h]},b={0};uint64_t word;
        assert(mng_lane_pack(&a,&word));assert(mng_lane_unpack(word,a.at,&b));
        assert(a.at==b.at && a.mask==b.mask && a.kind==b.kind && a.row==b.row &&
               a.core==b.core && a.svc==b.svc && a.hle==b.hle);
    }
    mng_lane_event a={0,0,1,0,0,0,0},b;uint64_t word;
    a.row=256;assert(!mng_lane_pack(&a,&word));a.row=0;
    a.mask=8;assert(!mng_lane_pack(&a,&word));a.mask=0;
    a.svc=256;assert(!mng_lane_pack(&a,&word));a.svc=0;
    assert(!mng_lane_unpack(1ull<<63,0,&b));assert(!mng_lane_unpack(0,0,&b));
    assert(mng_lane_pack(&a,&word));
    uint64_t x=mng_lane_hash_word(mng_lane_hash_word(MNG_LANE_HASH_SEED,word),123);
    assert(x!=mng_lane_hash_word(mng_lane_hash_word(MNG_LANE_HASH_SEED,123),word));
    printf("Compact lane packet round trips and validation passed; fixture=%llu\n",(unsigned long long)x);
}
