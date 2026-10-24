// SPDX-License-Identifier: MIT
// Android module loading and relocation follow games/totk/runtime/loader.c.
// Bionic key discovery follows D2R Mobile 758f5d4, validated per process/thread.
#include "guest_runtime.h"
#include "../third_party/d2r/sha256.h"
#include "expected_modules.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern "C" {
void runtime_gate();
uint64_t mng_call_guest(void* function, uintptr_t stack, uint64_t argument);
#ifdef MNG_STARTUP_PROBE
uint64_t mng_call_guest_startup(void* function, uintptr_t stack, const uint64_t* arguments);
#endif
__attribute__((visibility("hidden"))) uint64_t runtime_hle_tsd_off;
}
namespace {
using Json = nlohmann::json;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
size_t page_size() {
    const auto page = sysconf(_SC_PAGESIZE);
    require(page == 4096 || page == 16384, "Unsupported host page size");
    return size_t(page);
}
size_t up(size_t value) { const auto p = page_size(); return (value + p - 1) & ~(p - 1); }
uint64_t now_ns() {
    timespec t{}; require(clock_gettime(CLOCK_MONOTONIC, &t) == 0, "Monotonic clock unavailable");
    return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
uintptr_t platform_x18() { uintptr_t v; asm volatile("mov %0, x18" : "=r"(v)); return v; }
uintptr_t tsd_base() {
    uintptr_t tp; asm volatile("mrs %0, tpidr_el0" : "=r"(tp));
    uintptr_t base; memcpy(&base, reinterpret_cast<void*>(tp - 8), sizeof base); return base;
}
struct Region { uintptr_t start, end; int protection; };
std::vector<Region> regions() {
    std::ifstream maps("/proc/self/maps"); require(bool(maps), "Cannot inspect process mappings");
    std::vector<Region> result; std::string line;
    while (std::getline(maps, line)) {
        unsigned long start, end; char p[5]{};
        if (sscanf(line.c_str(), "%lx-%lx %4s", &start, &end, p) == 3)
            result.push_back({start, end, (p[0]=='r'?PROT_READ:0)|(p[1]=='w'?PROT_WRITE:0)|(p[2]=='x'?PROT_EXEC:0)});
    }
    return result;
}
size_t readable_size(uintptr_t address) {
    for (const auto& r : regions()) if (address >= r.start && address < r.end && (r.protection & PROT_READ)) return r.end - address;
    return 0;
}
struct Keys {
    std::array<pthread_key_t,9> keys{};
    std::array<uint64_t,9> offsets{};
    size_t allocated{};
    Keys() {
        try {
            for (auto& key : keys) { require(pthread_key_create(&key,nullptr)==0,"Cannot allocate thread key"); ++allocated; }
            int markers[2]{};
            require(pthread_setspecific(keys[0],&markers[0])==0 && pthread_setspecific(keys[8],&markers[1])==0,"Cannot set discovery keys");
            const auto base=tsd_base(); const auto readable=std::min<size_t>(readable_size(base),16384);
            require(readable>=16,"Bionic TLS base is unreadable");
            uint64_t found=0; unsigned matches=0;
            for (uint64_t at=8; at+8<=readable; at+=8) {
                uintptr_t value; memcpy(&value,reinterpret_cast<void*>(base+at),8);
                if(value==reinterpret_cast<uintptr_t>(&markers[0])) { found=at; ++matches; }
            }
            const auto index=[](pthread_key_t k){return uint64_t(uint32_t(k)&0x7fffffff);};
            require(matches==1 && found>=index(keys[0])*16+8,"Cannot derive Bionic pthread-key layout");
            const auto table=found-index(keys[0])*16-8;
            for(size_t i=0;i<keys.size();++i) {
                offsets[i]=table+index(keys[i])*16+8;
                require(offsets[i]+8<=readable,"Bionic key value outside readable region");
            }
            check(0); check(8);
            pthread_setspecific(keys[0],nullptr); pthread_setspecific(keys[8],nullptr);
        } catch (...) { release(); throw; }
    }
    void release() { for(size_t i=0;i<allocated;++i) pthread_key_delete(keys[i]); allocated=0; }
    ~Keys() { release(); }
    void set(size_t index, void* value) { require(pthread_setspecific(keys.at(index),value)==0,"Cannot set guest TLS"); }
    void* get(size_t index) const { return pthread_getspecific(keys.at(index)); }
    void check(size_t index) const {
        const auto base=tsd_base(); require(offsets.at(index)+8<=readable_size(base),"Thread key layout outside mapping");
        uintptr_t actual; memcpy(&actual,reinterpret_cast<void*>(base+offsets[index]),8);
        require(actual==reinterpret_cast<uintptr_t>(get(index)),"Bionic key layout mismatch on this thread");
    }
};
struct Mapping {
    uint8_t* base{}; size_t size{};
    explicit Mapping(size_t bytes, bool guest=false):size(up(bytes)) {
        void* p=MAP_FAILED;
        if (guest) {
            // Horizon IPC encodes guest pointers in 39 bits. A hint never
            // overwrites an existing mapping, including on older kernels.
            // Reject a relocated hint rather than accepting a high address.
            static std::mutex low_lock;
            std::lock_guard lock(low_lock);
            for (uintptr_t at=4ull<<30; at+size<(1ull<<39); at+=512ull<<20) {
                p=mmap(reinterpret_cast<void*>(at),size,PROT_READ|PROT_WRITE,
                       MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
                if (p==reinterpret_cast<void*>(at)) break;
                if (p!=MAP_FAILED) munmap(p,size);
                p=MAP_FAILED;
            }
        } else p=mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        require(p!=MAP_FAILED,"Cannot allocate module or stack mapping"); base=static_cast<uint8_t*>(p);
    }
    ~Mapping() { if(base) munmap(base,size); }
    Mapping(const Mapping&)=delete; Mapping& operator=(const Mapping&)=delete;
    void protect(size_t offset,size_t bytes,int prot) {
        require(offset<=size && bytes<=size-offset && !(prot&PROT_WRITE && prot&PROT_EXEC),"Invalid mapping protection");
        if(bytes) require(mprotect(base+offset,bytes,prot)==0,"Cannot set mapping protection");
    }
};
struct File {
    int fd{-1}; size_t size{};
    File(const std::string& path,size_t maximum) {
        fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        require(fd>=0,"Cannot open "+path);
        struct stat st{};
        if(fstat(fd,&st)!=0 || !S_ISREG(st.st_mode) || st.st_size<=0 || uint64_t(st.st_size)>maximum) {
            close(fd); fd=-1; throw std::runtime_error("Invalid file size/type: "+path);
        }
        size=size_t(st.st_size);
    }
    ~File(){if(fd>=0)close(fd);}
    void read_all(void* out) {
        size_t done=0; while(done<size) { auto n=pread(fd,static_cast<char*>(out)+done,size-done,done); if(n<0 && errno==EINTR)continue;require(n>0,"Short module read"); done+=size_t(n); }
    }
};
std::string sha256(const void* data,size_t bytes) {
    unsigned char digest[32]; require(bytes<=UINT32_MAX,"Digest input too large");
    CC_SHA256(data,CC_LONG(bytes),digest); char text[65];
    for(unsigned i=0;i<32;++i)snprintf(text+i*2,3,"%02x",digest[i]); return text;
}
struct Symbol { std::string name; uint64_t value,size; unsigned defined,binding,type; };
struct Reloc { uint64_t where; unsigned kind,symbol; int64_t addend; };
struct Gate { uint64_t ret; unsigned timer,arg; uint64_t original; };
struct Module {
    std::string name,build_id; size_t file_size{},text{},ro{},data{},key{},patch{};
    std::unique_ptr<Mapping> mapping; std::vector<Symbol> symbols; std::vector<Reloc> relocs;
    std::vector<Gate> gates; uint64_t relative{},symbolic{},cross{},weak{};
    uintptr_t address(uint64_t offset) const { require(offset<key,"Symbol outside module: "+name); return uintptr_t(mapping->base)+offset; }
};
struct Session;
struct Worker { Session* session; uintptr_t host_begin,host_end; uint64_t calls{},sleeps{}; };
thread_local Worker* active_worker;
#ifdef MNG_STARTUP_PROBE
[[noreturn]] void startup_callback();
#endif
struct Session {
    std::string title,manifest_hash;
    Keys keys;
    std::vector<Module> modules;
    std::unordered_map<std::string,uintptr_t> exports;
    std::unordered_map<uintptr_t,Gate> gates;
    uint64_t plt{},slot{}; std::string clock_symbol;
    std::atomic<uint64_t> calls{};
    std::atomic<uint64_t> sleeps{};
    std::mutex error_mutex; std::string worker_error;
#ifdef MNG_STARTUP_PROBE
    std::string startup_symbol;
    std::array<uint64_t,4> startup_entropy{};
    Json startup_services=Json::array();
    [[noreturn]] void stop_startup(const std::string& reason, uint64_t ret=0, const uint64_t* regs=nullptr) {
        Json result=report("sdk_startup");
        result["status"]="blocked";result["reason"]=reason;result["entry_vendor_symbol"]=startup_symbol;
        result["completed_services"]=startup_services;
        result["scope"]="Isolated SDK startup probe; no constructor, nnMain or graphics execution";
        if (ret) {
            for(const auto& m:modules)for(const auto& gate:m.gates)if(uintptr_t(m.mapping->base)+gate.ret==ret) {
                Json location={{"module",m.name},{"build_id",m.build_id},{"module_offset",gate.original},
                    {"guest_address",m.address(gate.original)},{"svc",gate.arg},{"registers",Json::array()}};
                if(regs)for(unsigned i=0;i<8;++i)location["registers"].push_back(regs[i]);
                location["vendor_symbol"]=nullptr;
                location["unknown_reason"]="No containing defined STT_FUNC symbol with a nonzero size";
                for(const auto& symbol:m.symbols)if(symbol.defined && symbol.type==2 && symbol.size &&
                    gate.original>=symbol.value && gate.original-symbol.value<symbol.size) {
                    location["vendor_symbol"]=symbol.name;
                    location["function_offset"]=gate.original-symbol.value;
                    location.erase("unknown_reason");break;
                }
                result["stop"]=std::move(location);
            }
        }
        const auto text=result.dump()+"\n";
        size_t done=0;while(done<text.size()) {const auto n=write(STDOUT_FILENO,text.data()+done,text.size()-done);if(n<0&&errno==EINTR)continue;if(n<=0)break;done+=size_t(n);}
        // This path exists only in the standalone executable. The OS reclaims
        // all guest mappings; unwinding across guest assembly is unnecessary.
        _exit(3);
    }
    [[noreturn]] void startup() {
        startup_symbol=title=="botw160"?"_ZN2nn4init5StartEmmPFvvES2_":"_ZN2nn4init5StartEmmPFvvES2_S2_";
        const auto entry=exports.find(startup_symbol);require(entry!=exports.end(),"Original SDK Start export missing");
        arc4random_buf(startup_entropy.data(),sizeof(startup_entropy));
        Mapping guest(2u<<20,true),host(2u<<20),environment(16384,true);
        for(auto* stack:{&guest,&host}){stack->protect(0,page_size(),PROT_NONE);stack->protect(stack->size-page_size(),page_size(),PROT_NONE);}
        Worker context{this,uintptr_t(host.base)+page_size(),uintptr_t(host.base)+host.size-page_size()};active_worker=&context;
        keys.set(0,nullptr);keys.set(1,environment.base);keys.set(2,environment.base+8192);
        keys.set(3,reinterpret_cast<void*>(runtime_gate));keys.set(8,reinterpret_cast<void*>(context.host_end-64));
        for(size_t i=0;i<keys.keys.size();++i)keys.check(i);
        // Reserve a main-thread identity for the future thread service table.
        // No unsupported thread query is acknowledged by this probe.
        const uint64_t callback=reinterpret_cast<uintptr_t>(startup_callback);
        const std::array<uint64_t,8> args={0x100,0,callback,callback,callback,0,0,0};
        mng_call_guest_startup(reinterpret_cast<void*>(entry->second),uintptr_t(guest.base)+guest.size-page_size()-64,args.data());
        stop_startup("SDK Start unexpectedly returned");
    }
#endif

    Session(const std::string& directory,const std::string& title):title(title) {
        require(title=="botw160" || title=="totk100","Unsupported title profile");
        const auto expected=Json::parse(title=="botw160"?mng_expected_botw:mng_expected_totk).at("exefs_sha256");
        File manifest(directory+"/manifest.json",64u<<20); std::string text(manifest.size,'\0'); manifest.read_all(text.data());
        const auto profile=Json::parse(mng_expected_android).at("profiles").at(title);
        manifest_hash=sha256(text.data(),text.size());
        require(manifest_hash==profile.at("manifest_sha256").get<std::string>(),"Module manifest does not match this runtime build");
        const auto doc=Json::parse(text);
        require(doc.at("schema_version")==1 && doc.at("target")=="android" && doc.at("title")==title,"Package target/title mismatch");
        require(doc.at("modules").is_array() && doc.at("modules").size()==3,"Expected three modules");
        const std::array<std::string,3> names={"main","subsdk0","sdk"};
        for(size_t index=0; index<names.size(); ++index) {
            const auto& j=doc.at("modules").at(index); Module m; m.name=j.at("name");
            require(m.name==names[index] && j.at("source_sha256")==expected.at(m.name),"Unsupported NSO identity");
            m.build_id=j.at("build_id");require(m.build_id.size()==64,"Invalid module Build ID");
            const auto& l=j.at("layout"); require(l.at("TARGET_ANDROID")==1,"Apple module rejected");
            m.text=l.at("TEXT_END");m.ro=l.at("RO_OFF");m.data=l.at("DATA_OFF");m.key=l.at("KEY_PAGE_OFF");m.patch=l.at("PATCH_OFF");
            m.file_size=j.at("file_size");
            require(m.file_size>16384 && m.file_size<512u*1024*1024 && m.key<=m.file_size-16384 && 0<m.text && m.text<=m.ro && m.ro<=m.data && m.data<m.key && m.key+16384<=m.patch && m.patch<m.file_size,"Invalid module layout");
            require(m.key%16384==0 && m.patch%16384==0 && up(m.text)<=(m.data&~(page_size()-1)),"Unaligned module segments");
            const std::array<const char*,8> key_names={"KEY_K18_IDX","KEY_KTLS_IDX","KEY_KTLR_IDX","KEY_KGATE_IDX","KEY_KRES0_IDX","KEY_KRES1_IDX","KEY_KASPECT_IDX","KEY_KASPECTUI_IDX"};
            for(size_t k=0;k<key_names.size();++k)require(l.at(key_names[k])==k,"Unsupported key-page ABI");
            require(j.at("file")==m.name+".patched.bin","Unexpected module filename");
            File file(directory+"/"+m.name+".patched.bin",512u<<20);require(file.size==m.file_size,"Module size mismatch");
            m.mapping=std::make_unique<Mapping>(m.file_size,true);file.read_all(m.mapping->base);
            require(sha256(m.mapping->base,m.file_size)==j.at("sha256").get<std::string>(),"Rewritten module SHA-256 mismatch: "+m.name);
            require(j.at("symbols").is_array() && j.at("symbols").size()<=1000000,"Invalid symbol count");
            for(const auto& s:j.at("symbols")) {
                require(s.is_array() && s.size()==6,"Invalid symbol record");
                Symbol symbol{s.at(0),s.at(1),s.at(2),s.at(3),s.at(4),s.at(5)};
                require(symbol.name.size()<=4096 && symbol.name.find('\0')==std::string::npos,"Invalid symbol name");m.symbols.push_back(std::move(symbol));
            }
            require(j.at("relocs").is_array() && j.at("relocs").size()<=3000000,"Invalid relocation count");
            std::set<uint64_t> seen;
            for(const auto& r:j.at("relocs")) {
                require(r.is_array() && r.size()==4,"Invalid relocation record");
                Reloc reloc{r.at(0),r.at(1),r.at(2),r.at(3)};
                require(reloc.where%8==0 && reloc.where>=m.data && reloc.where<=m.key-8 && reloc.symbol<m.symbols.size() && seen.insert(reloc.where).second,"Invalid relocation target");
                require(reloc.kind==257 || reloc.kind==1025 || reloc.kind==1026 || reloc.kind==1027,"Unsupported relocation kind");
                if(reloc.kind==1027)require(reloc.addend>=0 && uint64_t(reloc.addend)<m.key,"Relative addend outside image");
                m.relocs.push_back(reloc);
            }
            for(const auto& g:j.at("gates")) {
                require(g.is_array() && g.size()==4,"Invalid gate record");Gate gate{g.at(0),g.at(1),g.at(2),g.at(3)};
                require(gate.ret%4==0 && gate.ret>=m.patch && gate.ret<=m.file_size-4 && gate.original%4==0 && gate.original<m.text && gate.timer<=1,"Invalid gate address");
                require(!gate.timer || (gate.arg<31 && gate.arg!=18),"Invalid timer target register");m.gates.push_back(gate);
            }
            memcpy(m.mapping->base+m.key,keys.offsets.data(),8*sizeof(uint64_t));
            if(index==0) {
                const auto& c=j.at("clock_probe");plt=c.at("plt");slot=c.at("slot");clock_symbol=c.at("symbol");
                require(clock_symbol=="_ZN2nn2os13GetSystemTickEv" && plt%4==0 && plt+16<=m.text && slot>=m.data && slot<=m.key-8,"Invalid clock probe");
            }
            modules.push_back(std::move(m));
        }
        for(const auto& m:modules) {
            for(const auto& symbol:m.symbols) if(symbol.defined && symbol.value<m.key)exports.emplace(symbol.name,m.address(symbol.value));
            for(const auto& gate:m.gates)require(gates.emplace(uintptr_t(m.mapping->base)+gate.ret,gate).second,"Duplicate gate");
        }
        for(auto& m:modules) for(const auto& r:m.relocs) {
            uintptr_t value=0;
            if(r.kind==1027) {value=m.address(r.addend);++m.relative;}
            else {
                const auto& symbol=m.symbols.at(r.symbol);
                if(symbol.defined)value=m.address(symbol.value);
                else if(auto it=exports.find(symbol.name);it!=exports.end()){value=it->second;++m.cross;}
                else {require(symbol.binding==2,"Unresolved strong import: "+m.name+":"+symbol.name);++m.weak;}
                value+=r.kind==1026?0:r.addend;++m.symbolic;
            }
            memcpy(m.mapping->base+r.where,&value,8);
        }
        uintptr_t clock; memcpy(&clock,modules[0].mapping->base+slot,8);
        require(exports.contains(clock_symbol) && clock==exports.at(clock_symbol),"Clock GOT identity mismatch");
        // Re-derive the PLT target from the actual mapped instruction words.
        uint32_t words[4];memcpy(words,modules[0].mapping->base+plt,16);
        int64_t imm=((words[0]>>29)&3)|(((words[0]>>5)&0x7ffff)<<2);imm=(imm^(1<<20))-(1<<20);
        require((words[0]&0x9f00001f)==0x90000010 && (words[1]&0xffc003ff)==0xf9400211 && words[3]==0xd61f0220 && int64_t(plt&~4095ull)+imm*4096+((words[1]>>10)&0xfff)*8==int64_t(slot),"Original PLT pattern mismatch");
        runtime_hle_tsd_off=keys.offsets[8];
        for(auto& m:modules) seal(m);
    }
    void seal(Module& m) {
        const auto text_end=up(m.text),ro_end=m.data&~(page_size()-1);
        m.mapping->protect(0,text_end,PROT_READ|PROT_EXEC);
        m.mapping->protect(text_end,ro_end-text_end,PROT_READ);
        m.mapping->protect(m.key,16384,PROT_READ);
        m.mapping->protect(m.patch,m.mapping->size-m.patch,PROT_READ|PROT_EXEC);
        __builtin___clear_cache(reinterpret_cast<char*>(m.mapping->base),reinterpret_cast<char*>(m.mapping->base+m.mapping->size));
        const auto maps=regions();
        for(size_t at=0;at<m.mapping->size;at+=page_size()) {
            const auto address=uintptr_t(m.mapping->base)+at;
            const int expected=at<text_end||at>=m.patch?PROT_READ|PROT_EXEC:
                (at<ro_end||(at>=m.key&&at<m.key+16384))?PROT_READ:PROT_READ|PROT_WRITE;
            const auto r=std::find_if(maps.begin(),maps.end(),[&](auto& v){return address>=v.start&&address+page_size()<=v.end;});
            require(r!=maps.end() && r->protection==expected,"Module mapping permissions mismatch");
        }
    }
    void worker(unsigned number) {
        try {
            Mapping guest(256u<<10,true),host(2u<<20),environment(16384,true);
            for(auto* stack:{&guest,&host}){stack->protect(0,page_size(),PROT_NONE);stack->protect(stack->size-page_size(),page_size(),PROT_NONE);}
            auto* tls=environment.base;auto* tlr=environment.base+8192;
            Worker context{this,uintptr_t(host.base)+page_size(),uintptr_t(host.base)+host.size-page_size()};active_worker=&context;
            keys.set(0,reinterpret_cast<void*>(uintptr_t(0x123400+number)));
            keys.set(1,tls);keys.set(2,tlr);keys.set(3,reinterpret_cast<void*>(runtime_gate));
            keys.set(8,reinterpret_cast<void*>(context.host_end-64));
            for(size_t i=0;i<keys.keys.size();++i)keys.check(i);
            auto entry=reinterpret_cast<void*>(modules[0].address(plt));uint64_t previous=0;
            const auto x18=platform_x18();
            for(unsigned i=0;i<1000;++i) {
                auto value=mng_call_guest(entry,uintptr_t(guest.base)+guest.size-page_size()-64,0);
                require(value>=previous && value>0,"Guest clock is not monotonic");previous=value;
                require(platform_x18()==x18,"Platform x18 changed across guest call");
            }
            require(context.calls==1000,"Per-thread gate count mismatch");
            auto sleep=exports.find("_ZN2nn2os11SleepThreadENS_8TimeSpanE");
            require(sleep!=exports.end(),"Original SDK SleepThread missing");
            const auto start=now_ns();
            mng_call_guest(reinterpret_cast<void*>(sleep->second),uintptr_t(guest.base)+guest.size-page_size()-64,15000000);
            require(now_ns()-start>=15000000 && context.sleeps==1,"SDK sleep returned before its deadline");
            require(platform_x18()==x18,"Platform x18 changed across SDK sleep");
            require(keys.get(0)==reinterpret_cast<void*>(uintptr_t(0x123400+number)) && keys.get(1)==tls && keys.get(2)==tlr && keys.get(8)==reinterpret_cast<void*>(context.host_end-64),"Thread slots or host stack were not restored");
        } catch(const std::exception& e) {std::lock_guard lock(error_mutex);worker_error=e.what();}
        active_worker=nullptr;for(size_t i=0;i<keys.keys.size();++i)keys.set(i,nullptr);
    }
    Json report(const char* phase) {
        Json result={{"status","pass"},{"title",title},{"manifest_sha256",manifest_hash},{"phase",phase},{"host_page_size",page_size()},{"guest_address_bits",39},{"modules",Json::array()}};
        for(const auto& m:modules)result["modules"].push_back({{"name",m.name},{"build_id",m.build_id},{"base",uintptr_t(m.mapping->base)},{"relative",m.relative},{"symbolic",m.symbolic},{"cross",m.cross},{"weak_zero",m.weak}});
        return result;
    }
    Json run() {
        std::vector<std::jthread> threads;
        for(unsigned i=0;i<4;++i)threads.emplace_back([this,i]{worker(i);});
        for(auto& t:threads)t.join();require(worker_error.empty(),worker_error);require(calls==4000,"Expected 4000 native timer gates");
        require(sleeps==4,"Expected four original SDK sleep calls");
        Json result=report("native_calls");
        result.update({{"execution","native ARM64 main PLT -> SDK -> host timer; SDK SleepThread -> host wait"},{"calls",calls.load()},{"sleep_calls",sleeps.load()},{"threads",4},{"scope","Constructors, general Horizon services and game entry were not run"}});
        return result;
    }
};
std::mutex execution_mutex;
#ifdef MNG_STARTUP_PROBE
[[noreturn]] void startup_callback() {
    active_worker->session->stop_startup("SDK reached its first loader callback; module initialization is not connected");
}
#endif
}
extern "C" __attribute__((visibility("hidden"))) void runtime_dispatch(uint64_t ret,uint64_t* regs) {
    // The application supports timer and positive SleepThread gates. The
    // standalone startup probe adds only explicitly implemented bootstrap SVCs.
    // Unsupported service calls fail rather than returning fabricated success.
    auto* w=active_worker;uintptr_t local=uintptr_t(&w);
    if(!w || local<w->host_begin || local>=w->host_end || w->session->keys.get(8)!=nullptr) {
        fprintf(stderr,"Android gate did not switch to its host stack\n");abort();
    }
    auto& s=*w->session;auto g=s.gates.find(ret);
    if(g==s.gates.end()) {fprintf(stderr,"Unknown guest gate at %#llx\n",(unsigned long long)ret);abort();}
#ifdef MNG_STARTUP_PROBE
    if(!s.startup_symbol.empty() && !g->second.timer && g->second.arg==0x29 &&
       regs[1]==11 && regs[2]==0 && regs[3]<s.startup_entropy.size()) {
        // Existing services.c identifies this query as RandomEntropy. Keep
        // four process-scoped words, supplied by Bionic's system random source.
        // Never publish those values in the diagnostic report.
        if(s.startup_services.size()>=256)s.stop_startup("Startup service-call budget exceeded",ret,regs);
        s.startup_services.push_back({{"svc",0x29},{"info_id",11},{"sub_id",regs[3]}});
        regs[1]=s.startup_entropy[regs[3]];regs[0]=0;return;
    }
#endif
    if(g->second.timer) {
#ifdef MNG_STARTUP_PROBE
        if(!s.startup_symbol.empty() && w->calls>=10000)s.stop_startup("Startup timer-call budget exceeded",ret,regs);
#endif
        regs[g->second.arg]=uint64_t((__uint128_t(now_ns())*12)/625);
        ++w->calls;s.calls.fetch_add(1,std::memory_order_relaxed);
    } else if(g->second.arg==0x0b && regs[0]>0 && regs[0]<=1000000000) {
        // Same positive-duration semantics as the existing Horizon thread host.
        // Retry interrupted waits. Yield variants need the virtual scheduler.
        timespec left{time_t(regs[0]/1000000000),long(regs[0]%1000000000)};
        while(nanosleep(&left,&left)!=0)if(errno!=EINTR)abort();
        regs[0]=0;++w->sleeps;s.sleeps.fetch_add(1,std::memory_order_relaxed);
    } else {
#ifdef MNG_STARTUP_PROBE
        if(!s.startup_symbol.empty())s.stop_startup("Unsupported startup SVC",ret,regs);
#endif
        fprintf(stderr,"Unsupported guest SVC %#x at %#llx\n",g->second.arg,(unsigned long long)ret);abort();
    }
}
namespace mng {
#ifdef MNG_STARTUP_PROBE
[[noreturn]] void startup_probe(const std::string& directory,const std::string& title) {
    try { Session session(directory,title);session.startup(); }
    catch(const std::exception& e) {
        const auto report=Json({{"status","fail"},{"phase","sdk_startup"},{"error",e.what()}}).dump();
        fprintf(stdout,"%s\n",report.c_str());fflush(stdout);_exit(2);
    }
}
#endif
std::string validate_modules(const std::string& directory,const std::string& title) {
    std::lock_guard lock(execution_mutex);
    try { Session session(directory,title);return session.report("validated").dump(); }
    catch(const std::exception& e) {return Json({{"status","fail"},{"error",e.what()}}).dump();}
}
std::string probe_modules(const std::string& directory,const std::string& title) {
    std::lock_guard lock(execution_mutex);
    try { Session session(directory,title);return session.run().dump(); }
    catch(const std::exception& e) {return Json({{"status","fail"},{"error",e.what()}}).dump();}
}
}
