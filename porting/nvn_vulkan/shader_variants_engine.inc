// SPDX-License-Identifier: MIT
// Included inside the pinned renderer namespace after load_shader_key().
struct ShaderFamily {
    std::string identity;
    uint32_t stage{};
    Shader *single{};
    mng::shader::Family variants;
};
std::map<std::string,std::unique_ptr<ShaderFamily>> shader_families;
ShaderFamily *load_shader_family(const void *control,uint64_t code) {
    char identity[17];need(!mng_nvn_shader_key(control,code,identity),"Invalid shader code identity");
    auto found=shader_families.find(identity);if(found!=shader_families.end())return found->second.get();
    auto family=std::make_unique<ShaderFamily>();family->identity=identity;
    std::vector<uint8_t> bytes;
    if(mng_shader_read(identity,".xgv",bytes)){
        need(bytes.size()>=16 && bytes.size()<=65536,"Invalid shader variant index size");
        need(mng::shader::parse(bytes,family->variants),"Invalid shader variant index");family->stage=family->variants.stage;
        xbg_logf("NVN shader family %s stage=%u variants=%zu",identity,family->stage,family->variants.variants.size());
    }else{family->single=load_shader_key(identity);family->stage=family->single->stage;}
    auto *out=family.get();shader_families.emplace(identity,std::move(family));return out;
}
int shader_scalar(int f) {
    switch(f){
    case 3:case 8:case 11:case 15:case 20:case 23:case 27:case 32:case 35:case 39:case 44:case 47:case 0x3e:return 2;
    case 4:case 9:case 12:case 16:case 21:case 24:case 28:case 33:case 36:case 40:case 45:case 48:return 1;
    default:return 0;
    }
}
int shader_answer(const mng::shader::Condition &c) {
    if(c.kind==0){va_st attr;memcpy(&attr,&state.attrib[c.slot],sizeof(attr));return attr.fmt?shader_scalar(attr.fmt):-1;}
    auto *texture=state.rt.color[c.slot];if(!texture || c.slot>=uint32_t(state.rt.n))return 0;
    const auto &view=state.rt.cview[c.slot];return shader_scalar(state.rt.has_cview[c.slot]&&view.set_format?view.format:texture->d.format);
}
void resolve_shader_variants(bool compute) {
    for(unsigned stage=compute?5:0;stage<(compute?6:5);++stage){
        auto *family=state.shader_family[stage];if(!family)continue;
        if(family->single){state.shader[stage]=family->single;continue;}
        auto *selected=mng::shader::select(family->variants,shader_answer);
        if(!selected){
            for(const auto& row:family->variants.variants)for(const auto& c:row.conditions)
                xbg_logf("Shader variant state family=%s kind=%u slot=%u want=%u have=%d",family->identity.c_str(),c.kind,c.slot,c.scalar,shader_answer(c));
            xbg_fatalf("No precompiled shader variant matches draw state: %s",family->identity.c_str());
        }
        Shader *shader=load_shader_key(selected->key);need(shader->stage==stage,"Shader variant stage mismatch");state.shader[stage]=shader;
    }
}
