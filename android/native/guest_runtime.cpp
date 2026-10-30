#include "profiler.h"
#include "profile_c.h"
#include "guest_tags.h"
#include "debugbus.h"
// SPDX-License-Identifier: MIT
// Android module loading and relocation follow games/totk/runtime/loader.c.
// Bionic key discovery follows D2R Mobile 758f5d4, validated per process/thread.
#include "guest_runtime.h"
#include "expected_modules.h"
#include "port_runtime.h"
#include "guest_memory.h"
#ifdef MNG_GAME_SERVICES
#include "exclusive_acquire.h"
#include "render_resolution.h"
#include "realtime.h"
#include "../../games/totk/runtime/system_mode.h"
#endif
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
void mng_guest_loader_ready();
#endif
#ifdef MNG_STARTUP_PROBE
extern uint64_t runtime_hle_tsd_off;
pthread_key_t runtime_keys[8];
uint32_t svc_thread_main_init();
int svc_thread(int, uint64_t*);
int runtime_ipc_svc(unsigned, uint64_t*);
void runtime_core_checkpoint();
void* runtime_hle_wrap_named(void*, const char*);
void* runtime_hle_wrap_profile_marker(void*, const char*);
uint32_t runtime_thread_current_handle();
#ifdef MNG_GAME_SERVICES
void* runtime_override(const char*);
#endif
#else
__attribute__((visibility("hidden"))) uint64_t runtime_hle_tsd_off;
#endif
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
    return mng::port::sha256({static_cast<const uint8_t*>(data),bytes});
}
struct Symbol { std::string name; uint64_t value,size; unsigned defined,binding,type; };
struct Reloc { uint64_t where; unsigned kind,symbol; int64_t addend; };
struct Gate { uint64_t ret; unsigned timer,arg; uint64_t original; };
struct Module {
    std::string name,build_id,source_sha256,image_sha256; size_t file_size{},text{},ro{},data{},key{},patch{},timing{},tags{};
    std::unique_ptr<Mapping> mapping; std::vector<Symbol> symbols; std::vector<Reloc> relocs;
    Json guest_patches=Json::array();
    std::vector<Gate> gates; uint64_t relative{},symbolic{},cross{},weak{},init{},ctor_flag{},init_count{};
    uintptr_t address(uint64_t offset) const { require(offset<key,"Symbol outside module: "+name); return uintptr_t(mapping->base)+offset; }
};
struct Session;
struct Worker { Session* session; uintptr_t host_begin,host_end; uint64_t calls{},sleeps{},last_ret{}; const uint64_t* last_regs{}; };
thread_local Worker* active_worker;
#ifdef MNG_STARTUP_PROBE
void startup_callback();
Session* startup_session;
#endif
struct Session {
    std::string title,manifest_hash;
    Keys keys;
    std::vector<Module> modules;
    struct TagOwner { bool installed{}; ~TagOwner(){if(installed)mng_guest_tags_close();} } tag_owner;
    std::unique_ptr<mng::botw::RealtimeSession> realtime;
    std::unordered_map<std::string,uintptr_t> exports;
    std::unordered_map<uintptr_t,Gate> gates;
    uint64_t plt{},slot{}; std::string clock_symbol;
    std::atomic<uint64_t> calls{};
    std::atomic<uint64_t> sleeps{};
    std::mutex error_mutex; std::string worker_error;
    std::unique_ptr<mng::port::GuestMemory> memory;
    // Clear debugger bindings before module mappings and TLS keys are destroyed.
    mng::port::DebugSession debug;
    void publish_debug(const char* phase, Json detail=Json::object()) {
        if(!debug.generation())return;
        Json payload={{"schema_version",1},{"kind","mng-native-port"},{"profile",title},
            {"platform","android-arm64"},{"process_id",getpid()},{"generation",debug.generation()},
            {"phase",phase},{"manifest_sha256",manifest_hash},{"modules",Json::array()},{"detail",std::move(detail)}};
        for(const auto& m:modules)payload["modules"].push_back({{"module_index",&m-modules.data()},{"name",m.name},{"build_id",m.build_id},
            {"source_sha256",m.source_sha256},{"image_sha256",m.image_sha256},{"base",uintptr_t(m.mapping->base)},
            {"size",m.mapping->size},{"text_size",m.text},{"patch_offset",m.patch},{"guest_patches",m.guest_patches}});
        auto text=payload.dump();debug.publish(text);mng::diagnostics::setRuntime(std::move(text));
    }
#ifdef MNG_STARTUP_PROBE
    std::string startup_symbol;
    std::array<uint64_t,4> startup_entropy{};
    // Initial guest main-thread policy, matching the existing native host.
    // Additional guest threads are registered as the SDK creates them.
    uint32_t main_handle{};
    std::mutex startup_mutex;
    struct ThreadState { Mapping environment{16384,true}; Worker context{}; };
    std::vector<std::unique_ptr<ThreadState>> thread_states;
    static constexpr uint32_t main_priority=44;
    static constexpr uint64_t main_core_mask=7;
    Json startup_services=Json::array();
    [[noreturn]] void stop_startup(const std::string& reason, uint64_t ret=0, const uint64_t* regs=nullptr) {
        Json result=report("sdk_startup");
        result["status"]="blocked";result["reason"]=reason;result["entry_vendor_symbol"]=startup_symbol;
        result["completed_services"]=startup_services;
        result["scope"]="Native SDK startup; completed stage is recorded separately; no gameplay success implied";
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
        publish_debug("startup_blocked",result);
        const auto text=result.dump()+"\n";
        size_t done=0;while(done<text.size()) {const auto n=write(STDOUT_FILENO,text.data()+done,text.size()-done);if(n<0&&errno==EINTR)continue;if(n<=0)break;done+=size_t(n);}
        // Both the standalone probe and the APK's dedicated :game process own
        // their guest mappings. Never unwind C++ through guest assembly.
        _exit(3);
    }
    [[noreturn]] void startup() {
        startup_session=this;
        for(size_t i=0;i<8;++i)runtime_keys[i]=keys.keys[i];
        startup_symbol=title=="botw160"?"_ZN2nn4init5StartEmmPFvvES2_":"_ZN2nn4init5StartEmmPFvvES2_S2_";
        const auto entry=exports.find(startup_symbol);require(entry!=exports.end(),"Original SDK Start export missing");
        arc4random_buf(startup_entropy.data(),sizeof(startup_entropy));
        Mapping guest(2u<<20,true),host(2u<<20),environment(16384,true);
        for(auto* stack:{&guest,&host}){stack->protect(0,page_size(),PROT_NONE);stack->protect(stack->size-page_size(),page_size(),PROT_NONE);}
        mng::port::MemoryConfig config;
        for(const auto& m:modules)config.image_size+=m.key;
        memory=std::make_unique<mng::port::GuestMemory>(config);
        for(const auto& m:modules) {
            const auto base=uintptr_t(m.mapping->base),text_end=up(m.text),ro_end=m.data&~(page_size()-1);
            memory->register_mapping(base,text_end,3,5);
            if(ro_end>text_end)memory->register_mapping(base+text_end,ro_end-text_end,3,1);
            if(m.key>ro_end)memory->register_mapping(base+ro_end,m.key-ro_end,4,3);
            memory->register_mapping(base+m.key,16384,4,1);
            if(m.patch>m.key+16384)memory->register_mapping(base+m.key+16384,m.patch-m.key-16384,4,3);
            memory->register_mapping(base+m.patch,(m.timing?m.timing:(m.tags?m.tags:m.mapping->size))-m.patch,3,5);
            if(m.tags)memory->register_mapping(base+m.tags,16384,3,5);
            if(m.timing)memory->register_mapping(base+m.timing,16384,4,3);
        }
        memory->register_mapping(uintptr_t(guest.base)+page_size(),guest.size-2*page_size(),0xb,3);
        memory->register_mapping(uintptr_t(environment.base),environment.size,0xc,3);
        Worker context{this,uintptr_t(host.base)+page_size(),uintptr_t(host.base)+host.size-page_size()};active_worker=&context;
        keys.set(0,nullptr);keys.set(1,environment.base);keys.set(2,environment.base+8192);
        keys.set(3,reinterpret_cast<void*>(runtime_gate));keys.set(8,reinterpret_cast<void*>(context.host_end-64));
        for(size_t i=0;i<keys.keys.size();++i)keys.check(i);
        publish_debug("sdk_startup",{{"host_thread_id",gettid()},{"guest_stack_low",uintptr_t(guest.base)+page_size()},
            {"guest_stack_high",uintptr_t(guest.base)+guest.size-page_size()},{"guest_tls",uintptr_t(environment.base)},
            {"guest_tlr",uintptr_t(environment.base)+8192},{"guest_x18_slot",tsd_base()+keys.offsets[0]},
            {"platform_x18",platform_x18()},{"entry_vendor_symbol",startup_symbol}});
        main_handle=svc_thread_main_init();
        const uint64_t callback=reinterpret_cast<uintptr_t>(runtime_hle_wrap_named(reinterpret_cast<void*>(startup_callback),"module_initializers"));
        const std::array<uint64_t,8> args={main_handle,0,reinterpret_cast<uintptr_t>(mng_guest_loader_ready),callback,callback,0,0,0};
        mng_call_guest_startup(reinterpret_cast<void*>(entry->second),uintptr_t(guest.base)+guest.size-page_size()-64,args.data());
        stop_startup("SDK Start unexpectedly returned");
    }
#endif

    Session(const std::string& directory,const std::string& title):title(title),
        debug(title=="botw160" && getenv("MNG_PORT_DEBUG") && std::string(getenv("MNG_PORT_DEBUG"))=="1") {
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
            m.source_sha256=j.at("source_sha256");m.image_sha256=j.at("sha256");
            const auto& l=j.at("layout"); require(l.at("TARGET_ANDROID")==1,"Apple module rejected");
            m.text=l.at("TEXT_END");m.ro=l.at("RO_OFF");m.data=l.at("DATA_OFF");m.key=l.at("KEY_PAGE_OFF");m.patch=l.at("PATCH_OFF");
            m.file_size=j.at("file_size");
            require(m.file_size>16384 && m.file_size<512u*1024*1024 && m.key<=m.file_size-16384 && 0<m.text && m.text<=m.ro && m.ro<=m.data && m.data<m.key && m.key+16384<=m.patch && m.patch<m.file_size,"Invalid module layout");
            require(m.key%16384==0 && m.patch%16384==0 && up(m.text)<=(m.data&~(page_size()-1)),"Unaligned module segments");
            const std::array<const char*,8> key_names={"KEY_K18_IDX","KEY_KTLS_IDX","KEY_KTLR_IDX","KEY_KGATE_IDX","KEY_KRES0_IDX","KEY_KRES1_IDX","KEY_KASPECT_IDX","KEY_KASPECTUI_IDX"};
            for(size_t k=0;k<key_names.size();++k)require(l.at(key_names[k])==k,"Unsupported key-page ABI");
            require(j.at("file")==m.name+".patched.bin","Unexpected module filename");
            File file(directory+"/"+m.name+".patched.bin",512u<<20);require(file.size==m.file_size,"Module size mismatch");
            #ifdef MNG_GAME_SERVICES
            if(title=="botw160" && m.name=="main" && !(getenv("MNG_REALTIME") && !strcmp(getenv("MNG_REALTIME"),"0")))
                m.timing=(m.file_size+0x3fff)&~size_t(0x3fff);
#endif
#ifdef MNG_GAME_SERVICES
            if(title=="botw160" && m.name=="main" && mng_guest_tags_requested())
                m.tags=((m.file_size+0x3fff)&~size_t(0x3fff))+(m.timing?16384:0);
#endif
            m.mapping=std::make_unique<Mapping>(m.tags?m.tags+16384:(m.timing?m.timing+16384:m.file_size),true);file.read_all(m.mapping->base);
            require(sha256(m.mapping->base,m.file_size)==j.at("sha256").get<std::string>(),"Rewritten module SHA-256 mismatch: "+m.name);
#ifdef MNG_GAME_SERVICES
            if(title=="botw160" && m.name=="main") {
                const auto receipt=mng::botw::apply_exclusive_acquire({m.mapping->base,m.file_size},
                    {title,"android-arm64",m.name,m.build_id,m.source_sha256,m.image_sha256},m.text,m.patch);
                m.guest_patches.push_back({{"id","botw160.exclusive-acquire"},{"sites",receipt.applied.size()},
                    {"input_sha256",receipt.input_sha256},{"output_sha256",receipt.output_sha256},
                    {"stage","private_rw_before_relocation"}});
                fprintf(stderr,"[botw] exclusive-acquire: %zu sites, output %s\n",
                    receipt.applied.size(),receipt.output_sha256.c_str());
                auto timing_input=receipt.output_sha256;
                if(runtime_system_mode()->height==1080) {
                    const auto feature=mng::botw::apply_render_1080({m.mapping->base,m.file_size},
                        {title,"android-arm64",m.name,m.build_id,m.source_sha256,receipt.output_sha256});
                    m.guest_patches.push_back({{"id","botw160.render-1080"},{"sites",feature.applied.size()},
                        {"input_sha256",feature.input_sha256},{"output_sha256",feature.output_sha256},
                        {"stage","private_rw_before_relocation"}});
                    timing_input=feature.output_sha256;
                    fprintf(stderr,"[botw] render resolution 1920x1080 internal_scale=1 output=%s\n",feature.output_sha256.c_str());
                }
                if(m.timing) {
                    const auto timing=mng::botw::apply_realtime({m.mapping->base,m.file_size},
                        {title,"android-arm64",m.name,m.build_id,m.source_sha256,timing_input},m.timing);
                    m.guest_patches.push_back({{"id","botw160.realtime-vfr+pad-repeat+movie-pacing"},
                        {"sites",timing.applied.size()},{"input_sha256",timing.input_sha256},
                        {"output_sha256",timing.output_sha256},{"page_offset",m.timing},
                        {"stage","private_rw_before_relocation"}});
                    fprintf(stderr,"[botw] realtime 60fps: %zu guarded sites page=%zx output=%s\n",
                        timing.applied.size(),m.timing,timing.output_sha256.c_str());
                }
            }
#endif
#ifdef MNG_STARTUP_PROBE
            auto u32=[&](uint64_t off){require(off<=m.key-4,"Initializer metadata bounds");uint32_t v;memcpy(&v,m.mapping->base+off,4);return v;};
            auto u64=[&](uint64_t off){require(off<=m.key-8,"Initializer metadata bounds");uint64_t v;memcpy(&v,m.mapping->base+off,8);return v;};
            const auto mod=u32(4);require(u32(mod)==0x30444f4d,"Invalid initializer MOD0");
            const uint64_t dynamic=uint64_t(int64_t(mod)+int32_t(u32(mod+4)));
            bool ended=false;
            for(uint64_t at=dynamic;at<dynamic+65536;at+=16) {
                const auto tag=u64(at),value=u64(at+8);
                if(!tag){ended=true;break;}
                if(tag==12)m.init=value;
                if(tag==27){require(value%8==0,"Initializer array size");m.init_count=value/8;}
            }
            require(ended && m.init%4==0 && m.init+28<m.text,"Invalid initializer dynamic entry");
            const unsigned reg=title=="botw160"?19:20,load=title=="botw160"?20:24;
            const auto a=u32(m.init+16),b=u32(m.init+load);
            require((a&0x9f00001f)==(0x90000000|reg)&&(b&0xffc003ff)==(0xb9400008|(reg<<5)),"Unknown initializer state pattern");
            int64_t immediate=((a>>29)&3)|(((a>>5)&0x7ffff)<<2);immediate=(immediate^(1<<20))-(1<<20);
            m.ctor_flag=(m.init+16&~4095ull)+immediate*4096+((b>>10)&0xfff)*4;
            require(m.ctor_flag>=m.data&&m.ctor_flag<=m.key-4,"Initializer state outside data");
#endif
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
#ifdef MNG_GAME_SERVICES
                if(auto host=runtime_override(symbol.name.c_str()))value=reinterpret_cast<uintptr_t>(runtime_hle_wrap_named(host,symbol.name.c_str()));
                else
#endif
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
#ifdef MNG_GAME_SERVICES
        for(auto& m:modules)if(m.tags){
            if(!mng_guest_tags_install(m.mapping->base,m.file_size,m.tags,m.mapping->size,
                m.build_id.c_str(),m.source_sha256.c_str(),runtime_hle_wrap_profile_marker,runtime_thread_current_handle))
                throw std::runtime_error(mng_guest_tags_error());
            tag_owner.installed=true;
            m.guest_patches.push_back({{"id","botw160.performance-tags-v1"},{"sites",12},{"page_offset",m.tags},
                {"stage","private_rw_after_relocation"},{"default_active",false}});
            fprintf(stderr,"[botw] guest performance tags: 12 guarded sites page=%zx active=0\n",m.tags);
        }
#endif
        for(auto& m:modules) seal(m);
        for(auto& m:modules)if(m.timing)realtime=std::make_unique<mng::botw::RealtimeSession>(m.mapping->base,m.timing);
        publish_debug("modules_sealed");
    }
    void seal(Module& m) {
        const auto text_end=up(m.text),ro_end=m.data&~(page_size()-1);
        m.mapping->protect(0,text_end,PROT_READ|PROT_EXEC);
        m.mapping->protect(text_end,ro_end-text_end,PROT_READ);
        m.mapping->protect(m.key,16384,PROT_READ);
        m.mapping->protect(m.patch,(m.timing?m.timing:(m.tags?m.tags:m.mapping->size))-m.patch,PROT_READ|PROT_EXEC);
        if(m.tags)m.mapping->protect(m.tags,16384,PROT_READ|PROT_EXEC);
        __builtin___clear_cache(reinterpret_cast<char*>(m.mapping->base),reinterpret_cast<char*>(m.mapping->base+m.mapping->size));
        const auto maps=regions();
        for(size_t at=0;at<m.mapping->size;at+=page_size()) {
            const auto address=uintptr_t(m.mapping->base)+at;
            const int expected=at<text_end||(m.tags && at>=m.tags)||(at>=m.patch && (!m.timing || at<m.timing))?PROT_READ|PROT_EXEC:
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
        if(memory)result["memory"]={{"aslr_base",memory->base()},{"aslr_size",memory->size()},
            {"heap_base",memory->heap()},{"heap_size",memory->heap_size()},
            {"alias_base",memory->alias()},{"alias_bytes",memory->alias_bytes()},
            {"pool_size",*memory->info(6)},{"used_size",*memory->info(7)}};
        for(const auto& m:modules)result["modules"].push_back({{"name",m.name},{"build_id",m.build_id},{"base",uintptr_t(m.mapping->base)},{"relative",m.relative},{"symbolic",m.symbolic},{"cross",m.cross},{"weak_zero",m.weak},{"guest_patches",m.guest_patches}});
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
void startup_callback() {
    auto& s=*active_worker->session;
    s.publish_debug("module_initializers");
    for(auto& m:s.modules) {
        require(m.init && m.ctor_flag,"Missing verified module initializer");
        fprintf(stderr,"[android-boot] ENTER %s DT_INIT +%#llx\n",m.name.c_str(),(unsigned long long)m.init);
        // A nested guest call gets a low address guest stack and a separate
        // host stack. Do not put HIPC stack pointers above the guest VA limit.
        Mapping guest(2u<<20,true),host(2u<<20);
        s.memory->register_mapping(uintptr_t(guest.base),guest.size,0xb,3);
        Worker nested{&s,uintptr_t(host.base),uintptr_t(host.base)+host.size};
        auto* previous=active_worker;auto* slot=s.keys.get(8);
        active_worker=&nested;s.keys.set(8,reinterpret_cast<void*>(nested.host_end-64));
        const std::array<uint64_t,8> args{};
        mng_call_guest_startup(reinterpret_cast<void*>(m.address(m.init)),uintptr_t(guest.base)+guest.size-64,args.data());
        s.keys.set(8,slot);active_worker=previous;
        s.memory->unregister_mapping(uintptr_t(guest.base),guest.size);
        uint32_t state;memcpy(&state,reinterpret_cast<void*>(m.address(m.ctor_flag)),4);
        require(state==2,"Module constructors did not finish");
        fprintf(stderr,"[android-boot] RETURN %s DT_INIT state=%u count=%llu\n",m.name.c_str(),state,(unsigned long long)m.init_count);
    }
    s.publish_debug("module_initializers_complete");
}
#endif
}
#ifdef MNG_STARTUP_PROBE
extern "C" int mng_guest_memory_writable(uint64_t address,size_t bytes) {
    return startup_session && startup_session->memory && startup_session->memory->accessible(address,bytes,3);
}
extern "C" [[noreturn]] void runtime_fail(const char* reason) {
    if(active_worker) active_worker->session->stop_startup(reason);
    fprintf(stderr,"[android-native] %s\n",reason);_exit(3);
}
extern "C" uint64_t runtime_invoke_guest(void* function,const uint64_t* arguments,uint32_t count) {
    spatial::LiteTraceScope callback_scope("HLE.GuestCallback");
    if(!active_worker||count>8)runtime_fail("Invalid nested guest callback");
    auto& s=*active_worker->session;Mapping guest(2u<<20,true),host(2u<<20);
    s.memory->register_mapping(uintptr_t(guest.base),guest.size,0xb,3);
    Worker nested{&s,uintptr_t(host.base),uintptr_t(host.base)+host.size};
    auto* previous=active_worker;auto* slot=s.keys.get(8);active_worker=&nested;
    s.keys.set(8,reinterpret_cast<void*>(nested.host_end-64));
    std::array<uint64_t,8> args{};std::copy_n(arguments,count,args.begin());
    const auto result=mng_call_guest_startup(function,uintptr_t(guest.base)+guest.size-64,args.data());
    s.keys.set(8,slot);active_worker=previous;s.memory->unregister_mapping(uintptr_t(guest.base),guest.size);
    return result;
}
extern "C" uint64_t runtime_now_ns() { return now_ns(); }
extern "C" void* runtime_symbol(const char* name) {
    auto it=startup_session->exports.find(name);
    if(it==startup_session->exports.end())runtime_fail((std::string("Missing guest export: ")+name).c_str());
    return reinterpret_cast<void*>(it->second);
}
extern "C" int runtime_guest_code_pc(uint64_t pc) {
    for(const auto& m:startup_session->modules)if(pc>=uintptr_t(m.mapping->base)&&pc-uintptr_t(m.mapping->base)<m.text)return 1;
    return 0;
}
extern "C" void runtime_trace_guest(uint64_t fp,uint64_t lr) {
    for(unsigned i=0;i<20;++i) {
        bool found=false;
        for(const auto& m:startup_session->modules)if(lr>=uintptr_t(m.mapping->base)&&lr-uintptr_t(m.mapping->base)<m.mapping->size) {
            fprintf(stderr,"[guest] %s+%#llx\n",m.name.c_str(),(unsigned long long)(lr-uintptr_t(m.mapping->base)));found=true;break;
        }
        if(!found)fprintf(stderr,"[guest] host/unknown %#llx\n",(unsigned long long)lr);
        if(fp%16||!startup_session->memory->accessible(fp,16,1))break;
        uint64_t next[2];memcpy(next,reinterpret_cast<void*>(fp),16);if(next[0]==fp)break;fp=next[0];lr=next[1];
    }
}
extern "C" void runtime_thread_init() {
    auto& s=*startup_session;
    auto state=std::make_unique<Session::ThreadState>();
    pthread_attr_t attr;pthread_getattr_np(pthread_self(),&attr);
    void* base;size_t size;pthread_attr_getstack(&attr,&base,&size);pthread_attr_destroy(&attr);
    state->context={&s,uintptr_t(base),uintptr_t(base)+size};
    active_worker=&state->context;
    s.keys.set(0,nullptr);s.keys.set(1,state->environment.base);s.keys.set(2,state->environment.base+8192);
    s.keys.set(3,reinterpret_cast<void*>(runtime_gate));
    s.memory->register_mapping(uintptr_t(state->environment.base),state->environment.size,0xc,3);
    for(size_t i=0;i<4;++i)s.keys.check(i);
    std::lock_guard lock(s.startup_mutex);s.thread_states.push_back(std::move(state));
}
#endif
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
    if(!s.startup_symbol.empty() && !g->second.timer && s.memory) {
        w->last_ret=ret;w->last_regs=regs;
        const auto svc=g->second.arg;
        spatial::LiteTraceScope svc_scope(mng::profile::svcName(svc));
        spatial::LiteTraceScope guest_scope;
        if(mng_profile_guest_svc(svc) && mng_profile_guest_epoch()){
            guest_scope.begin("Guest.PatchedSVC");
            spatial::LiteTrace::counter("Guest.SVC",svc);
            for(size_t i=0;i<s.modules.size();++i){const auto &m=s.modules[i];
                const auto base=uintptr_t(m.mapping->base);
                if(ret>=base && ret-base<m.mapping->size){
                    spatial::LiteTrace::counter("Guest.Site.Module",i);
                    spatial::LiteTrace::counter("Guest.Site.Offset",g->second.original);break;}}
            // The gate's saved x30 is its own trampoline return, not a Guest caller.
            // Report bounded frame-record return addresses, without naming callees.
            static const char* module_names[]={"Guest.Return0.Module","Guest.Return1.Module","Guest.Return2.Module","Guest.Return3.Module"};
            static const char* offset_names[]={"Guest.Return0.Offset","Guest.Return1.Offset","Guest.Return2.Offset","Guest.Return3.Offset"};
            auto fp=regs[29];
            for(unsigned depth=0;depth<4;++depth){
                int module=-1;uint64_t offset=0,frame[2]{};
                const bool readable=fp && !(fp&15) && s.memory && s.memory->accessible(fp,16,1);
                if(readable){memcpy(frame,reinterpret_cast<const void*>(fp),16);
                    for(size_t i=0;i<s.modules.size();++i){const auto &m=s.modules[i];const auto base=uintptr_t(m.mapping->base);
                        if(frame[1]>=base && frame[1]-base<m.text){module=int(i);offset=frame[1]-base;break;}}}
                spatial::LiteTrace::counter(module_names[depth],module);
                spatial::LiteTrace::counter(offset_names[depth],offset);
                fp=readable && frame[0]>fp && frame[0]-fp<=(1u<<20)?frame[0]:0;
            }
        }
        bool handled=false;
        Json detail={{"svc",svc}};
        try {
            if(svc==0x29) {
                auto value=s.memory->info(uint32_t(regs[1]));
                if(!value) switch(uint32_t(regs[1])) {
                    case 0: value=7;break;
                    case 1: value=0x0ffffffff0000000ull;break;
                    case 8: case 10: case 28: value=0;break;
                    case 16: value=32ull<<20;break;
                    // This host has no Horizon kernel page tables charged to
                    // the title's system resource pool.
                    case 17: value=0;break;
                    case 18: value=0x01007ef00011e000ull;break;
                    case 20: value=uintptr_t(s.keys.get(2))+4096;break;
                    case 23: value=1;break;
                }
                if(value) {
                    detail["info_id"]=regs[1];detail["sub_id"]=regs[3];
                    // IsDebuggerAttached is a system-wide query and takes an invalid handle.
                    const bool system_query=uint32_t(regs[1])==8 || uint32_t(regs[1])==10;
                    if(uint32_t(regs[2])!=(system_query?0u:0xffff8001u))regs[0]=(114u<<9)|1;
                    else if(regs[3])regs[0]=(14u<<9)|1;
                    else {regs[0]=0;regs[1]=*value;}
                    handled=true;
                }
            } else if(svc==0x06) {
                detail["address"]=regs[2];
                if(!s.memory->accessible(regs[0],sizeof(mng::port::MemoryInfo),2))regs[0]=(115u<<9)|1;
                else {const auto info=s.memory->query(regs[2]);memcpy(reinterpret_cast<void*>(regs[0]),&info,sizeof(info));regs[0]=0;regs[1]=0;}
                handled=true;
            } else if(svc==0x01) {
                detail["size"]=regs[1];regs[0]=s.memory->set_heap_size(regs[1]);
                if(!regs[0])regs[1]=s.memory->heap();handled=true;
            } else if(svc==0x02) {
                detail["address"]=regs[0];detail["size"]=regs[1];detail["permission"]=regs[2];
                regs[0]=s.memory->set_permission(regs[0],regs[1],uint32_t(regs[2]));handled=true;
            } else if(svc==0x04 || svc==0x05) {
                detail["destination"]=regs[0];detail["source"]=regs[1];detail["size"]=regs[2];
                regs[0]=s.memory->map_stack(regs[0],regs[1],regs[2],svc==0x05);handled=true;
            } else if(svc==0x2c || svc==0x2d) {
                detail["address"]=regs[0];detail["size"]=regs[1];
                regs[0]=s.memory->map_physical(regs[0],regs[1],svc==0x2d);handled=true;
            } else if(svc==0x27) {
                require(regs[1]<=65536 && s.memory->accessible(regs[0],regs[1],1),"Invalid guest debug string");
                fwrite(reinterpret_cast<const void*>(regs[0]),1,size_t(regs[1]),stderr);regs[0]=0;handled=true;
            } else if(svc==0x26) {
                if(regs[0]&0x80000000ull){regs[0]=0;handled=true;}
                else s.stop_startup("Original SDK Break",ret,regs);
            } else if(svc_thread(int(svc),regs) || runtime_ipc_svc(svc,regs)) {
                runtime_core_checkpoint(); handled=true;
            }

        } catch(const std::exception& e) {s.stop_startup(std::string("Guest memory host failure: ")+e.what(),ret,regs);}
        if(handled) {

            spatial::LiteTraceScope bookkeeping_scope("HLE.SVC.RecordDiagnostic");
            std::lock_guard log_lock(s.startup_mutex);
            detail["result"]=regs[0];if(s.startup_services.size()<4096)s.startup_services.push_back(std::move(detail));return;
        }
    }
    if(!s.startup_symbol.empty() && !g->second.timer && g->second.arg==0x29 &&
       regs[1]==11 && regs[2]==0 && regs[3]<s.startup_entropy.size()) {
        // Existing services.c identifies this query as RandomEntropy. Keep
        // four process-scoped words, supplied by Bionic's system random source.
        // Never publish those values in the diagnostic report.
        std::lock_guard log_lock(s.startup_mutex);
        if(s.startup_services.size()<4096)s.startup_services.push_back({{"svc",0x29},{"info_id",11},{"sub_id",regs[3]}});
        regs[1]=s.startup_entropy[regs[3]];regs[0]=0;return;
    }
#endif
    if(g->second.timer) {
#ifdef MNG_STARTUP_PROBE

#endif
        spatial::LiteTraceScope timer_scope;if(mng::profile::detailedCpu())timer_scope.begin("HLE.CounterRead");
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
