// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mng::shader {
// Immutable little-endian host asset archive. No driver objects or guest
// addresses are stored. Writers publish a new file, never mutate a mapping.
class Archive {
    struct Header {char magic[8];uint32_t version,count;uint64_t bytes,index;};
    struct Entry {char name[24];uint64_t offset,bytes;};
    static_assert(sizeof(Header)==32 && sizeof(Entry)==40);
    const uint8_t *data_{};size_t bytes_{};std::vector<Entry> entries_;
public:
    explicit Archive(const char *path){
        int fd=open(path,O_RDONLY|O_CLOEXEC);
        if(fd<0)throw std::runtime_error("Cannot open shader archive");
        struct stat st{};const bool valid=!fstat(fd,&st) && st.st_size>=32 && uint64_t(st.st_size)<(16ull<<30);
        if(!valid){close(fd);throw std::runtime_error("Invalid shader archive extent");}
        bytes_=size_t(st.st_size);void *p=mmap(nullptr,bytes_,PROT_READ,MAP_PRIVATE,fd,0);close(fd);
        if(p==MAP_FAILED)throw std::runtime_error("Cannot map shader archive");
        data_=static_cast<const uint8_t*>(p);
        try {
            Header h;memcpy(&h,data_,sizeof h);
            if(memcmp(h.magic,"MNGSHD1",8) || h.version!=1 || h.bytes!=bytes_ || h.index!=32 ||
               h.count>200000 || uint64_t(h.count)*sizeof(Entry)>bytes_-32)
                throw std::runtime_error("Invalid shader archive index");
            entries_.resize(h.count);memcpy(entries_.data(),data_+32,entries_.size()*sizeof(Entry));
            uint64_t end=32+entries_.size()*sizeof(Entry);
            for(size_t i=0;i<entries_.size();++i){auto &e=entries_[i];
                bool name=true;for(unsigned j=0;j<16;++j)name &= (e.name[j]>='0'&&e.name[j]<='9') || (e.name[j]>='a'&&e.name[j]<='f');
                if(!name || (memcmp(e.name+16,".xgs\0\0\0",8) && memcmp(e.name+16,".xgv\0\0\0",8)) ||
                   (i && strcmp(entries_[i-1].name,e.name)>=0) || e.offset<end || e.offset>bytes_ ||
                   e.bytes>bytes_-e.offset || e.bytes>(16u<<20) || e.bytes<16)
                    throw std::runtime_error("Invalid shader archive entry");
                end=e.offset+e.bytes;
            }
        }catch(...){munmap(const_cast<uint8_t*>(data_),bytes_);data_=nullptr;throw;}
    }
    ~Archive(){if(data_)munmap(const_cast<uint8_t*>(data_),bytes_);}
    Archive(const Archive&)=delete;Archive& operator=(const Archive&)=delete;
    bool read(const std::string &name,std::vector<uint8_t>& out)const{
        auto it=std::lower_bound(entries_.begin(),entries_.end(),name,
            [](const Entry& e,const std::string& n){return strcmp(e.name,n.c_str())<0;});
        if(it==entries_.end() || name!=it->name)return false;
        out.assign(data_+it->offset,data_+it->offset+it->bytes);return true;
    }
    size_t count()const{return entries_.size();}
};
}
