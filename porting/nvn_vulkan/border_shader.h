// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace mng::shader {
struct BorderBinding {uint32_t binding, white_binding;};
// Filtering is affine in a non-comparison border color, including mip and
// anisotropic filtering. Two canonical samples preserve the complete footprint:
// black + custom * (white - black). This matches MacNative's filtered fallback.
inline std::vector<uint32_t> emulate_border(const std::vector<uint32_t>& input,
                                          const std::vector<BorderBinding>& requests, uint32_t color_binding) {
    auto require=[](bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);};
    require(input.size()>=5 && input[0]==0x07230203,"Invalid border shader SPIR-V");
    using Words=std::vector<uint32_t>;
    std::map<uint32_t,Words> types,variables;
    std::map<uint32_t,uint32_t> binding,sets;
    std::vector<Words> instructions;
    for(size_t at=5;at<input.size();){
        unsigned n=input[at]>>16,op=input[at]&65535;
        require(n && at+n<=input.size(),"Truncated border shader instruction");
        Words w(input.begin()+at,input.begin()+at+n);instructions.push_back(w);
        if(op>=19 && op<=39 && n>=2)types[w[1]]=w;
        if(op==59 && n>=4)variables[w[2]]=w;
        if(op==71 && n==4 && w[2]==33)binding[w[1]]=w[3];
        if(op==71 && n==4 && w[2]==34)sets[w[1]]=w[3];
        at+=n;
    }
    uint32_t next=input[3];
    auto id=[&](){require(next<0x3fffffff,"Border shader ID limit");return next++;};
    auto inst=[](unsigned op,std::initializer_list<uint32_t> args){Words w{uint32_t((args.size()+1)<<16)|op};w.insert(w.end(),args);return w;};
    std::map<uint32_t,size_t> selected;
    std::set<uint32_t> opaque_types;
    std::vector<uint32_t> white_vars;
    std::vector<Words> decorations,globals;
    for(size_t r=0;r<requests.size();++r){
        uint32_t var=0;
        for(auto [v,b]:binding)if(b==requests[r].binding && sets[v]==0)var=v;
        require(var && variables.count(var),"Border sampler variable missing");
        const auto& decl=variables.at(var);
        require(decl.size()==4 && decl[3]==0,"Border sampler must be a scalar UniformConstant variable");
        const auto& ptr=types.at(decl[1]);
        require((ptr[0]&65535)==32 && ptr.size()==4,"Border sampler pointer type");
        const auto& sampled=types.at(ptr[3]);
        require((sampled[0]&65535)==27,"Border fallback requires combined image sampler");
        opaque_types.insert(decl[1]);opaque_types.insert(ptr[3]);
        const auto& image=types.at(sampled[2]);
        require(image.size()>=9 && image[3]<=2 && image[4]==0 && image[6]==0,
                "Border fallback requires non-depth, non-MS 1D/2D/3D sampled image");
        const auto& scalar=types.at(image[2]);
        require((scalar[0]&65535)==22 && scalar[2]==32,"Border fallback requires float32 sampling");
        uint32_t white=id();white_vars.push_back(white);selected[var]=r;
        globals.push_back(inst(59,{decl[1],white,0}));
        decorations.push_back(inst(71,{white,34,0}));
        decorations.push_back(inst(71,{white,33,requests[r].white_binding}));
    }
    require(!requests.empty(),"Border fallback requires sampler bindings");
    uint32_t float4=0;
    for(const auto& [type,w]:types)if((w[0]&65535)==23 && w.size()==4 && w[3]==4){
        const auto& scalar=types.at(w[2]);if((scalar[0]&65535)==22 && scalar[2]==32){float4=type;break;}
    }
    require(float4,"Border shader float4 type missing");
    uint32_t uint_type=0,vec_ptr=0;
    for(const auto& [type,w]:types){
        if((w[0]&65535)==21 && w.size()==4 && w[2]==32 && w[3]==0)uint_type=type;
        if((w[0]&65535)==32 && w.size()==4 && w[2]==2 && w[3]==float4)vec_ptr=type;
    }
    if(!uint_type){uint_type=id();globals.push_back(inst(21,{uint_type,32,0}));}
    uint32_t count=id(),zero=id(),array=id(),block=id(),block_ptr=id(),color_var=id();
    globals.push_back(inst(43,{uint_type,count,uint32_t(requests.size())}));
    globals.push_back(inst(43,{uint_type,zero,0}));
    globals.push_back(inst(28,{array,float4,count}));
    globals.push_back(inst(30,{block,array}));
    globals.push_back(inst(32,{block_ptr,2,block}));
    if(!vec_ptr){vec_ptr=id();globals.push_back(inst(32,{vec_ptr,2,float4}));}
    globals.push_back(inst(59,{block_ptr,color_var,2}));
    decorations.push_back(inst(71,{array,6,16}));
    decorations.push_back(inst(71,{block,2}));
    decorations.push_back(inst(72,{block,0,35,0}));
    decorations.push_back(inst(71,{color_var,34,0}));
    decorations.push_back(inst(71,{color_var,33,color_binding}));
    std::vector<uint32_t> indices;
    for(size_t r=0;r<requests.size();++r){indices.push_back(id());globals.push_back(inst(43,{uint_type,indices.back(),uint32_t(r)}));}
    struct Loaded {size_t request;uint32_t white;};
    std::map<uint32_t,Loaded> loaded;
    std::set<uint32_t> extracted_images;
    std::vector<Words> rewritten;
    for(auto w:instructions){
        const unsigned op=w[0]&65535;
        // Reject unsupported opaque-value flow before it can silently leave a
        // custom-border sample using the canonical black sampler. Ordinary
        // image queries still use the original image and need no compensation.
        if((op==55 || op==169 || op==245) && w.size()>2)
            require(!opaque_types.count(w[1]),"Border sampler parameter/select/phi flow unsupported");
        if(op==57)for(size_t i=4;i<w.size();++i)
            require(!selected.count(w[i]) && !loaded.count(w[i]),"Border sampler function argument unsupported");
        if((op==65 || op==66 || op==67 || op==70 || op==83) && w.size()>3)
            require(!selected.count(w[3]),"Border sampler pointer alias unsupported");
        if(op==62 && w.size()>2)
            require(!selected.count(w[1]) && !loaded.count(w[2]),"Border sampler store unsupported");
        if(op==100 && w.size()>3 && loaded.count(w[3]))extracted_images.insert(w[2]);
        if(op==86 && w.size()>3)
            require(!extracted_images.count(w[3]),"Border sampler reconstruction unsupported");
        if(op>=305 && op<=316 && w.size()>3)
            require(!loaded.count(w[3]),"Sparse border sampling unsupported");
        if(op==61 && w.size()>=4 && selected.count(w[3])){
            rewritten.push_back(w);const size_t r=selected.at(w[3]);
            uint32_t black=w[2],white=id();w[2]=white;w[3]=white_vars[r];
            loaded[black]={r,white};rewritten.push_back(w);continue;
        }
        if(op==83 && w.size()==4 && loaded.count(w[3])){
            rewritten.push_back(w);auto source=loaded.at(w[3]);uint32_t black=w[2];
            w[2]=id();w[3]=source.white;loaded[black]={source.request,w[2]};rewritten.push_back(w);continue;
        }
        if(op>=87 && op<=97 && w.size()>=4 && loaded.count(w[3])){
            require(op==87 || op==88 || op==91 || op==92,"Border sample opcode requires separate implementation");
            auto sample=loaded.at(w[3]);const auto& type=types.at(w[1]);
            require((type[0]&65535)==23 && type.size()==4 && type[3]==4,"Border sample result must be float4");
            const auto& scalar=types.at(type[2]);
            require((scalar[0]&65535)==22 && scalar[2]==32,"Border sample result scalar type");
            require(w[1]==float4,"Border result vector type mismatch");
            uint32_t color_ptr=id(),color=id();
            rewritten.push_back(inst(65,{vec_ptr,color_ptr,color_var,zero,indices[sample.request]}));
            rewritten.push_back(inst(61,{float4,color,color_ptr}));
            uint32_t result=w[2],black=id(),white=id(),weight=id(),scaled=id();
            w[2]=black;rewritten.push_back(w);w[2]=white;w[3]=sample.white;rewritten.push_back(w);
            rewritten.push_back(inst(131,{w[1],weight,white,black}));
            rewritten.push_back(inst(133,{w[1],scaled,color,weight}));
            rewritten.push_back(inst(129,{w[1],result,black,scaled}));continue;
        }
        rewritten.push_back(w);
    }
    Words out(input.begin(),input.begin()+5);out[3]=next;
    auto append=[&](const Words& w){out.insert(out.end(),w.begin(),w.end());};
    bool decorated=false,global=false;
    for(auto w:rewritten){
        const unsigned op=w[0]&65535;
        if(!decorated && op>=19 && op<=39){for(const auto& d:decorations)append(d);decorated=true;}
        if(!global && op==54){for(const auto& g:globals)append(g);global=true;}
        if(op==15 && input[1]>=0x00010400){w.insert(w.end(),white_vars.begin(),white_vars.end());w.push_back(color_var);w[0]=uint32_t(w.size()<<16)|op;}
        append(w);
    }
    require(decorated && global,"Border shader lacks module sections");
    return out;
}
}
