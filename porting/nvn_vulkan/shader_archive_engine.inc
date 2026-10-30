// SPDX-License-Identifier: MIT
// A configured archive is authoritative for this process. Missing assets never
// silently fall back to a different loose bundle. Both readers use the same
// immutable bytes; normal shader header validation and overrides still apply.
bool mng_shader_read(const std::string& key,const char* extension,std::vector<uint8_t>& bytes){
    spatial::LiteTraceScope scope("NVN.Shader.Read");
    static std::unique_ptr<mng::shader::Archive> archive=[] {
        const char *path=getenv("MNG_SHADER_ARCHIVE");
        if(!path || !*path)return std::unique_ptr<mng::shader::Archive>{};
        try{return std::make_unique<mng::shader::Archive>(path);}
        catch(const std::exception& e){xbg_fatalf("Shader archive: %s",e.what());}
        return std::unique_ptr<mng::shader::Archive>{};
    }();
    const std::string name=key+extension;
    if(archive)return archive->read(name,bytes);
    FILE *f=fopen((std::string(cfg.shader_directory)+"/"+name).c_str(),"rb");
    if(!f){if(errno==ENOENT)return false;xbg_fatalf("Cannot open shader asset %s",name.c_str());}
    if(fseek(f,0,SEEK_END)){fclose(f);xbg_fatalf("Cannot seek shader asset %s",name.c_str());}
    long n=ftell(f);rewind(f);
    if(n<16 || n>16*1024*1024){fclose(f);xbg_fatalf("Invalid shader asset extent %s",name.c_str());}
    bytes.resize(size_t(n));bool ok=fread(bytes.data(),1,bytes.size(),f)==bytes.size();fclose(f);
    need(ok,"Shader asset read failed");return true;
}
