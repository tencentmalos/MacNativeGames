// SPDX-License-Identifier: MIT
#include "../nvn_vulkan/border_shader.h"
#include "../nvn_vulkan/tests/border_edge_spirv.h"
#include <fstream>
#include <iostream>
int main(int argc,char** argv){
    std::vector<uint32_t> input(std::begin(border_edge_spirv),std::end(border_edge_spirv));
    mng::shader::BorderBinding binding{4,10};
    auto result=mng::shader::emulate_border(input,{binding},16);
    if(result.size()<=input.size() || result[3]<=input[3])return 1;
    unsigned samples=0,subtract=0,multiply=0,add=0;
    for(size_t i=5;i<result.size();i+=result[i]>>16){auto op=result[i]&65535;samples+=op==87;subtract+=op==131;multiply+=op==133;add+=op==129;}
    if(samples!=2 || subtract!=1 || multiply!=1 || add!=1)return 2;
    bool rejected=false;try{auto bad=binding;bad.binding=99;(void)mng::shader::emulate_border(input,{bad},16);}catch(const std::exception&){rejected=true;}
    if(!rejected)return 3;
    // An opaque sampler passed to another function cannot be rewritten by the
    // local-load transform. Reject it instead of returning a black-border path.
    auto escaped=input;
    for(size_t i=5;i<escaped.size();i+=escaped[i]>>16)if((escaped[i]&65535)==87){
        const uint32_t call[]{(5u<<16)|57,escaped[i+1],escaped[3]++,1,escaped[i+3]};
        escaped.insert(escaped.begin()+i,std::begin(call),std::end(call));break;
    }
    rejected=false;try{(void)mng::shader::emulate_border(escaped,{binding},16);}catch(const std::exception&){rejected=true;}
    if(!rejected)return 5;
    if(argc==2){std::ofstream file(argv[1],std::ios::binary);file.write(reinterpret_cast<const char*>(result.data()),result.size()*4);if(!file)return 4;}
    std::cout<<"PASS: border shader rewrite, missing binding and opaque-flow rejection\n";
}
