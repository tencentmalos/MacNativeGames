extern "C" int mng_preparation_probe(char*,unsigned);
extern "C" int mng_nvn_schedule(int,int,int,int,int,int,int,int,int,int,char*,unsigned);
extern "C" int mng_pipeline_cache_probe(char*,unsigned);
extern "C" int mng_warm_pipeline_probe(char*,unsigned);
extern "C" int mng_nvn_test_registry_progress(const void*);
extern "C" int mng_nvn_test_flush_ordering(const void*);
extern "C" int mng_nvn_test_invalidate_ordering(const void*);
extern "C" void mng_nvn_test_record_event(void*,uint64_t,uint32_t);
// SPDX-License-Identifier: MIT
#include "nvn_vulkan.h"
#include "../runtime/host_clock.h"
#include <vulkan/vulkan_beta.h>
#include "vulkan_driver.h"
#include "tests/border_spirv.h"
#include "tests/border_edge_spirv.h"
#include "tests/array_sample_spirv.h"
#include "tests/packed_spirv.h"
#include "tests/increment_spirv.h"
#include "tests/dynamic_uniform_spirv.h"
#include "nvn_protos.h"
#include "tests/synthetic_spirv.h"
#include "engine/xbg_game_internal.h"
#include <filesystem>
#include <array>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
extern "C" int mng_nvn_shader_key(const void*,uint64_t,char[17]);
extern "C" uint64_t mng_nvn_range_preflight_resolves();
extern "C" uint64_t mng_nvn_batch_submissions();
extern "C" uint64_t mng_nvn_batch_deferred();
extern "C" uint64_t mng_nvn_pipeline_misses();
extern "C" uint64_t mng_nvn_render_pass_begins();

void need(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void vkcheck(VkResult r) {
    if (r != VK_SUCCESS)
        throw std::runtime_error("Vulkan call failed: " + std::to_string(r));
}
template <class T> T proc(const char *name) {
    auto p = mng_nvn_get_proc_address(name);
    need(p != nullptr, name);
    return reinterpret_cast<T>(p);
}
// Resolve once per call site, as Guest code does. Re-resolving every call
// would drain publications through GetProcAddress and hide batching bugs.
#define NVN(name) ([] {static const auto function=proc<NVNT_##name>(#name);return function;}())
#include "tests/ordered_handoff.h"
#include "tests/counter_handoff.h"
#include "tests/sync_wait.h"
#include "tests/present_handoff.h"
namespace {
uintptr_t owned_memory;size_t owned_size;
int validate_owned(uint64_t address,size_t bytes) {return address>=owned_memory && address-owned_memory<=owned_size && bytes<=owned_size-(address-owned_memory);}
// The probe is single-threaded here (before the present-worker test). Forward
// real Vulkan waits while observing whether texture retirement drains the GPU.
// Forwarding keeps the negative control safe and RAII restores Volk on failure.
struct FinalizeWaitAudit {
    static inline unsigned calls{};
    static inline PFN_vkQueueWaitIdle queue_wait{};
    static inline PFN_vkDeviceWaitIdle device_wait{};
    static inline PFN_vkWaitForFences fence_wait{};
    static VKAPI_ATTR VkResult VKAPI_CALL queue(VkQueue q) {++calls;return queue_wait(q);}
    static VKAPI_ATTR VkResult VKAPI_CALL device(VkDevice d) {++calls;return device_wait(d);}
    static VKAPI_ATTR VkResult VKAPI_CALL fences(VkDevice d,uint32_t n,const VkFence *f,VkBool32 all,uint64_t timeout) {
        ++calls;return fence_wait(d,n,f,all,timeout);
    }
    FinalizeWaitAudit() {
        calls=0;queue_wait=vkQueueWaitIdle;device_wait=vkDeviceWaitIdle;fence_wait=vkWaitForFences;
        vkQueueWaitIdle=queue;vkDeviceWaitIdle=device;vkWaitForFences=fences;
    }
    ~FinalizeWaitAudit() {vkQueueWaitIdle=queue_wait;vkDeviceWaitIdle=device_wait;vkWaitForFences=fence_wait;}
};
void command_accounting_test(NVNdevice *device) {
    NVNcommandBuffer commands[2]{};alignas(8) uint8_t control[2][256]{};
    for(unsigned i=0;i<2;++i){
        need(NVN(nvnCommandBufferInitialize)(&commands[i],device),"Accounting command initialize");
        NVN(nvnCommandBufferAddCommandMemory)(&commands[i],nullptr,0,4096);
        NVN(nvnCommandBufferAddControlMemory)(&commands[i],control[i],sizeof control[i]);
    }
    auto *c=&commands[0];NVN(nvnCommandBufferBeginRecording)(c);
    NVNvertexAttribState attributes[2]{};NVNvertexStreamState streams[2]{};NVNbufferRange ranges[2]{};
    NVN(nvnCommandBufferBindVertexAttribState)(c,2,attributes);
    NVN(nvnCommandBufferBindVertexStreamState)(c,2,streams);
    NVN(nvnCommandBufferBindVertexBuffers)(c,0,2,ranges);
    NVN(nvnCommandBufferBindUniformBuffer)(c,NVN_SHADER_STAGE_VERTEX,0,0,16);
    NVN(nvnCommandBufferBindStorageBuffer)(c,NVN_SHADER_STAGE_VERTEX,0,0,16);
    NVN(nvnCommandBufferBindTexture)(c,NVN_SHADER_STAGE_FRAGMENT,0,1);
    NVN(nvnCommandBufferBindImage)(c,NVN_SHADER_STAGE_FRAGMENT,0,1);
    NVN(nvnCommandBufferDrawElements)(c,NVN_DRAW_PRIMITIVE_TRIANGLES,NVN_INDEX_TYPE_UNSIGNED_SHORT,0,0);
    NVN(nvnCommandBufferDrawElementsBaseVertex)(c,NVN_DRAW_PRIMITIVE_TRIANGLES,NVN_INDEX_TYPE_UNSIGNED_SHORT,0,0,0);
    auto source=NVN(nvnCommandBufferEndRecording)(c);
    constexpr size_t expected=68+72+48+20+20+20+24+48+44;
    need(NVN(nvnCommandBufferGetCommandMemoryUsed)(c)==expected,"Guest command budget includes host payload capacity");
    need(NVN(nvnCommandBufferGetCommandMemoryFree)(c)==4096-expected,"Guest command free budget");
    NVN(nvnCommandBufferFinalize)(c);
    c=&commands[1];NVN(nvnCommandBufferBeginRecording)(c);
    NVN(nvnCommandBufferCopyCommands)(c,1,&source);NVN(nvnCommandBufferCallCommands)(c,1,&source);
    NVN(nvnCommandBufferEndRecording)(c);
    need(NVN(nvnCommandBufferGetCommandMemoryUsed)(c)==2*expected,"Copied command guest budget lost");
    NVN(nvnCommandBufferAddCommandMemory)(c,nullptr,0,4096);
    need(NVN(nvnCommandBufferGetCommandMemoryUsed)(c)==0,"Guest command budget reset");
    NVN(nvnCommandBufferAddCommandMemory)(c,nullptr,0,1u<<20);
    NVN(nvnCommandBufferBeginRecording)(c);
    NVN(nvnCommandBufferCopyCommands)(c,0,nullptr);
    std::array<NVNcommandHandle,256> repeated;repeated.fill(source);
    NVN(nvnCommandBufferCopyCommands)(c,int(repeated.size()),repeated.data());
    auto large=NVN(nvnCommandBufferEndRecording)(c);
    need(NVN(nvnCommandBufferGetCommandMemoryUsed)(c)==repeated.size()*expected,
         "Bulk command copy changed guest accounting across arena growth");
    NVN(nvnDeviceFinalizeCommandHandle)(device,large);
    NVN(nvnDeviceFinalizeCommandHandle)(device,source);
    NVN(nvnCommandBufferFinalize)(&commands[1]);
}
void concurrent_command_test(NVNdevice *device) {
    mng_nvn_resources before{};need(!mng_nvn_resource_stats(&before),"Command lifetime baseline");
    std::atomic<unsigned> ready{},completed{};std::atomic<bool> start{};
    std::vector<std::thread> workers;std::exception_ptr failure;std::mutex failure_mutex;
    for(unsigned worker=0;worker<8;++worker)workers.emplace_back([&]{
        ++ready;while(!start.load())std::this_thread::yield();
        try {
            for(unsigned iteration=0;iteration<512;++iteration){
                NVNtextureBuilder builder{};
                NVN(nvnTextureBuilderSetDefaults)(&builder);
                NVN(nvnTextureBuilderSetFormat)(&builder,NVN_FORMAT_RGBA8);
                NVN(nvnTextureBuilderSetSize2D)(&builder,8,8);
                need(NVN(nvnTextureBuilderGetStorageSize)(&builder)>=8*8*4,"Concurrent builder layout");
                NVNcommandBuffer command{};alignas(8) uint8_t control[256]{};
                need(NVN(nvnCommandBufferInitialize)(&command,device),"Concurrent command initialize");
                NVN(nvnCommandBufferAddCommandMemory)(&command,nullptr,0,4096);
                NVN(nvnCommandBufferAddControlMemory)(&command,control,sizeof control);
                NVN(nvnCommandBufferBeginRecording)(&command);
                NVN(nvnCommandBufferSetViewport)(&command,0,0,8,8);
                need(NVN(nvnCommandBufferEndRecording)(&command)!=0,"Concurrent command handle");
                need(NVN(nvnCommandBufferGetCommandMemoryUsed)(&command)>0,"Concurrent command budget");
                NVN(nvnCommandBufferFinalize)(&command);++completed;
            }
        }catch(...){std::lock_guard lock(failure_mutex);failure=std::current_exception();}
    });
    while(ready.load()!=8)std::this_thread::yield();start=true;
    for(auto &worker:workers)worker.join();
    if(failure)std::rethrow_exception(failure);
    need(completed==4096,"Concurrent command lifecycle completion");
    mng_nvn_resources after{};need(!mng_nvn_resource_stats(&after),"Command lifetime result");
    need(after.host_objects==before.host_objects,"Finalized recorders retained host objects");
}
void transient_builder_test(NVNdevice *device) {
    // A builder has no Finalize. More than the old registry limit must be safe.
    for(unsigned i=0;i<1048580;++i){
        alignas(8) uint8_t storage[144];memset(storage,0xa5,sizeof storage);
        memset(storage,0,128);auto *builder=reinterpret_cast<NVNtextureBuilder*>(storage);
        NVN(nvnTextureBuilderSetDevice)(builder,device);
        NVN(nvnTextureBuilderSetDefaults)(builder);
        NVN(nvnTextureBuilderSetFormat)(builder,NVN_FORMAT_RGBA8);
        NVN(nvnTextureBuilderSetSize2D)(builder,8,8);
        NVNtextureBuilder copy;memcpy(&copy,builder,128);
        NVN(nvnTextureBuilderSetSize2D)(builder,64,64);
        need(NVN(nvnTextureBuilderGetStorageSize)(&copy)==512,"Copied builder changed with its source");
        for(unsigned j=128;j<144;++j)need(storage[j]==0xa5,"Builder exceeded guest storage");
    }
}
void value_lifetime_test(NVNdevice *device,const NVNtextureBuilder *builder) {
    mng_nvn_resources before{},after{};
    need(!mng_nvn_resource_stats(&before),"Initial lifetime counters");
    NVNsamplerBuilder sb{};
    NVN(nvnSamplerBuilderSetDefaults)(&sb);
    NVN(nvnSamplerBuilderSetDevice)(&sb,device);
    alignas(8) uint8_t sampler_storage[64]{};memset(sampler_storage+48,0xa5,16);
    auto *sampler=reinterpret_cast<NVNsampler*>(sampler_storage);
    NVNsampler empty{};
    need(NVN(nvnSamplerInitialize)(sampler,&sb),"Inline sampler initialization");
    need(!NVN(nvnSamplerCompare)(sampler,&empty),"Empty sampler cannot compare equal");
    NVNsampler copy{};memcpy(&copy,sampler,48);
    NVN(nvnSamplerBuilderSetLodBias)(&sb,1.0f);
    NVN(nvnSamplerInitialize)(sampler,&sb);
    need(!NVN(nvnSamplerCompare)(sampler,&copy),"Sampler copy changed with source");
    memcpy(&copy,sampler,48);NVN(nvnSamplerFinalize)(&copy);
    need(NVN(nvnSamplerCompare)(sampler,sampler),"Sampler copy finalization changed source");
    for(unsigned j=48;j<64;++j)need(sampler_storage[j]==0xa5,"Sampler exceeded native storage");
    // Exceed the former process-wide cap with actual textures and samplers,
    // including new zeroed builders every time. A raised cap cannot pass the
    // constant host-object-count assertion.
    for(unsigned i=0;i<1048580;++i) {
        NVNtexture texture{};
        need(NVN(nvnTextureInitialize)(&texture,builder),"Million texture churn initialization");
        NVN(nvnTextureFinalize)(&texture);
        NVNsamplerBuilder transient{};NVNsampler value{};
        NVN(nvnSamplerBuilderSetDefaults)(&transient);
        NVN(nvnSamplerInitialize)(&value,&transient);
        need(NVN(nvnSamplerCompare)(&value,&value),"Transient sampler compare");
        NVN(nvnSamplerFinalize)(&value);
        NVNsync sync{};need(NVN(nvnSyncInitialize)(&sync,device),"Inline sync initialize");
        NVN(nvnSyncFinalize)(&sync);
        NVNbufferBuilder buffer{};NVN(nvnBufferBuilderSetDefaults)(&buffer);
        NVNmemoryPoolBuilder pool{};NVN(nvnMemoryPoolBuilderSetDefaults)(&pool);
        NVNqueueBuilder queue{};NVN(nvnQueueBuilderSetDefaults)(&queue);
        NVNwindowBuilder window{};NVN(nvnWindowBuilderSetDefaults)(&window);
    }
    need(!mng_nvn_resource_stats(&after),"Final lifetime counters");
    need(after.host_objects==before.host_objects,"Value churn retained host objects");
    need(after.texture_initializations-before.texture_initializations==1048580 &&
         after.texture_finalizations-before.texture_finalizations==1048580,"Texture lifetime accounting");
    fprintf(stderr,"[lifetime-test] texture_cycles=1048580 host_objects_before=%llu after=%llu\n",
        (unsigned long long)before.host_objects,(unsigned long long)after.host_objects);
}
void virtual_pool_test(NVNdevice *device) {
    constexpr size_t page=65536,bytes=2*page;
    void *reservation=mmap(nullptr,bytes+page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    need(reservation!=MAP_FAILED,"Physical test reservation");
    struct Cleanup {void *p;~Cleanup(){munmap(p,bytes+page);owned_memory=owned_size=0;mng_nvn_set_memory_validator(nullptr);}} cleanup{reservation};
    owned_memory=(uintptr_t(reservation)+page-1)&~uintptr_t(page-1);owned_size=bytes;
    auto *physical=reinterpret_cast<uint8_t*>(owned_memory);physical[0]=17;physical[page]=31;
    mng_nvn_set_memory_validator(validate_owned);
    NVNmemoryPoolBuilder builder{};NVNmemoryPool backing{},virtual_pool{};
    NVN(nvnMemoryPoolBuilderSetDefaults)(&builder);NVN(nvnMemoryPoolBuilderSetDevice)(&builder,device);
    NVN(nvnMemoryPoolBuilderSetStorage)(&builder,physical,bytes);
    NVN(nvnMemoryPoolBuilderSetFlags)(&builder,NVN_MEMORY_POOL_FLAGS_PHYSICAL|NVN_MEMORY_POOL_FLAGS_CPU_CACHED|NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS);
    need(NVN(nvnMemoryPoolInitialize)(&backing,&builder),"Physical pool memfd import");
    NVN(nvnMemoryPoolBuilderSetStorage)(&builder,nullptr,bytes);
    NVN(nvnMemoryPoolBuilderSetFlags)(&builder,NVN_MEMORY_POOL_FLAGS_VIRTUAL|NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    need(NVN(nvnMemoryPoolInitialize)(&virtual_pool,&builder),"Virtual pool reservation");
    struct Request {const NVNmemoryPool *pool;int64_t physical,virtual_offset;uint64_t size;int32_t storage,padding;};
    auto map=[&](int count,Request *requests){return NVN(nvnMemoryPoolMapVirtual)(&virtual_pool,count,reinterpret_cast<NVNmappingRequest*>(requests));};
    Request requests[]={{&backing,page,0,page,0,0},{&backing,0,page,page,0,0}};
    need(map(2,requests),"Virtual alias page mapping");
    auto *alias=reinterpret_cast<uint8_t*>(NVN(nvnMemoryPoolGetBufferAddress)(&virtual_pool));
    need(!mng_nvn_ordered_probe_report(uintptr_t(physical)) && !mng_nvn_ordered_probe_report(uintptr_t(alias)),"Unproved physical report alias accepted");
    need(alias[0]==31 && alias[page]==17,"Mapping preserves pre-import content");
    physical[page+9]=72;need(alias[9]==72,"Physical-to-virtual coherence");
    alias[page+13]=99;need(physical[13]==99,"Virtual-to-physical coherence");
    requests[0].physical=0;requests[1].virtual_offset=bytes;
    need(!map(2,requests) && alias[0]==31,"Invalid mapping batch must not partially apply");
    Request unmap{nullptr,0,0,page,0,0};need(map(1,&unmap),"Virtual page unmap");
    need(alias[0]==0 && physical[page]==31,"Unmapping preserves physical backing");
    NVN(nvnMemoryPoolFinalize)(&virtual_pool);NVN(nvnMemoryPoolFinalize)(&backing);
}
}
int mng_nvn_self_test(const char *directory, char *report, size_t report_bytes) {
    static std::mutex run_mutex;
    std::lock_guard lock(run_mutex);
    VkInstance instance{};
    VkDevice device{};
    bool initialized = false;
    void *storage = nullptr;
    std::string synthetic_directory;
    try {
        need(directory && report && report_bytes, "Missing self-test arguments");
        struct stat st {};
        need(stat(directory, &st) == 0 && S_ISDIR(st.st_mode), "Missing private shader directory");
        {
            std::array<uint8_t,0x880> control{};
            const uint32_t bytes=8;memcpy(control.data()+0x704,&bytes,4);
            uint8_t first[8]{},second[8];memset(second,1,sizeof second);
            char a[17]{},b[17]{};
            need(!mng_nvn_shader_key(control.data(),uintptr_t(first),a) &&
                 !mng_nvn_shader_key(control.data(),uintptr_t(second),b),"Shader code identity");
            need(!strcmp(a,"91a28d75eae0c464") && !strcmp(b,"61bd70fcf60a9af1"),
                 "Equal controls with different code must not share a shader key");
        }
        std::string pattern = std::string(directory) + "/mng-synthetic-XXXXXX";
        std::vector<char> path(pattern.begin(), pattern.end());
        path.push_back(0);
        need(mkdtemp(path.data()) != nullptr, "Synthetic shader directory allocation");
        synthetic_directory = path.data();
        std::array<std::array<uint8_t, 0x880>, 10> controls{};
        auto shader_bundle = [&](unsigned index, unsigned stage, const uint32_t *words,
                                 size_t bytes) {
            controls[index][0] = uint8_t(0xf0 + stage);
            if(index>=3)controls[index][1]=uint8_t(index);
            char key[17]{};
            need(xbg_shader_control_key(controls[index].data(), key) == 0,
                 "Synthetic shader identity");
            xbg_shader_header header{0x31534758, 1, stage,           index==9 ? 2u : stage == 5 || index==3 || index==7 || index==8 ? 1u : 0u,
                                     index==4 ? 1u : 0u, 0, uint32_t(bytes), 0};
            std::ofstream output(synthetic_directory + "/" + key + ".xgs", std::ios::binary);
            output.write(reinterpret_cast<const char *>(&header), sizeof(header));
            if (stage == 5) {
                xbg_shader_binding binding{
                    0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, 256, XBG_SOURCE_STORAGE, 0, 0, 0};
                output.write(reinterpret_cast<const char *>(&binding), sizeof(binding));
            }
            if(index==3 || index==7 || index==8){
                xbg_shader_binding binding{4,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,0,XBG_SOURCE_TEXTURE,0,UINT32_MAX,(1u<<31)|(1u<<8)|(index==8?1u<<12:0)};
                output.write(reinterpret_cast<const char*>(&binding),sizeof(binding));
            }
            if(index==9){
                // Intentionally noncanonical metadata order; dynamic offsets
                // must follow Vulkan binding/array order, not this file order.
                const xbg_shader_binding bindings[]{
                    {16,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,16,XBG_SOURCE_UNIFORM,3,0,0},
                    {4,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2,16,XBG_SOURCE_UNIFORM,7,0,0}};
                output.write(reinterpret_cast<const char*>(bindings),sizeof bindings);
            }
            if(index==4){const uint32_t location=0;output.write(reinterpret_cast<const char*>(&location),4);}
            output.write(reinterpret_cast<const char *>(words), bytes);
            need(bool(output), "Synthetic shader bundle write");
        };
        shader_bundle(0, 0, triangle_spirv, sizeof(triangle_spirv));
        shader_bundle(1, 4, green_spirv, sizeof(green_spirv));
        shader_bundle(2, 5, storage_spirv, sizeof(storage_spirv));
        shader_bundle(3, 4, border_spirv, sizeof(border_spirv));
        shader_bundle(4, 0, packed_vert_spirv, sizeof(packed_vert_spirv));
        shader_bundle(5, 4, packed_frag_spirv, sizeof(packed_frag_spirv));
        shader_bundle(6, 5, increment_spirv, sizeof(increment_spirv));
        shader_bundle(7, 4, border_edge_spirv, sizeof(border_edge_spirv));
        shader_bundle(8, 4, array_sample_spirv, sizeof(array_sample_spirv));
        shader_bundle(9,4,dynamic_uniform_spirv,sizeof(dynamic_uniform_spirv));
        // BOTW control records can end exactly at a protected page boundary.
        // Their key must exclude following allocation bytes, unlike D2R's
        // fixed-size control record. This also catches accidental overreads.
        {
            size_t page=size_t(sysconf(_SC_PAGESIZE));
            void *memory=mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            need(memory!=MAP_FAILED,"Shader key guard allocation");
            auto *control=static_cast<uint8_t*>(memory)+page-0x7d1;
            memset(control,17,0x7d1);uint32_t length=0x7d1;memcpy(control+0x20,&length,4);
            need(!mprotect(static_cast<uint8_t*>(memory)+page,page,PROT_NONE),"Shader key guard page");
            char key[17]{};need(!xbg_shader_control_key(control,key),"Short control key");
            need(!strcmp(key,"e42d801539e988da"),"Short control SHA mismatch");munmap(memory,page*2);
        }
        if(const char* error=mng_vulkan_initialize(nullptr,nullptr,nullptr))throw std::runtime_error(error);
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "MngNvnProbe";
        application.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &application;
#if defined(__APPLE__)
        const char* portability=VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        uint32_t instance_count{};vkcheck(vkEnumerateInstanceExtensionProperties(nullptr,&instance_count,nullptr));
        std::vector<VkExtensionProperties> instance_extensions(instance_count);
        vkcheck(vkEnumerateInstanceExtensionProperties(nullptr,&instance_count,instance_extensions.data()));
        for(const auto& e:instance_extensions)if(!strcmp(e.extensionName,portability)) {
            ci.flags|=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
            ci.enabledExtensionCount=1;ci.ppEnabledExtensionNames=&portability;
        }
#endif
        vkcheck(vkCreateInstance(&ci, nullptr, &instance));
        volkLoadInstance(instance);
        uint32_t n = 0;
        vkcheck(vkEnumeratePhysicalDevices(instance, &n, nullptr));
        need(n > 0, "No Vulkan devices");
        std::vector<VkPhysicalDevice> physical(n);
        vkcheck(vkEnumeratePhysicalDevices(instance, &n, physical.data()));
        VkPhysicalDevice gpu = physical.front();
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(gpu, &properties);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, families.data());
        uint32_t family = 0;
        while (family < n &&
               (families[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) !=
                   (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            ++family;
        need(family < n, "No graphics+compute queue");
        float priority = 1;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &queue_info;
        VkPhysicalDeviceFeatures supported{},enabled{};
        vkGetPhysicalDeviceFeatures(gpu,&supported);
        enabled.textureCompressionBC=supported.textureCompressionBC;
        enabled.depthBounds=supported.depthBounds;
        di.pEnabledFeatures=&enabled;
        uint32_t extension_count=0;
        vkcheck(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&extension_count,nullptr));
        std::vector<VkExtensionProperties> extensions(extension_count);
        vkcheck(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&extension_count,extensions.data()));
        bool has_border=false;for(const auto& e:extensions)if(!strcmp(e.extensionName,VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME))has_border=true;
        VkPhysicalDeviceCustomBorderColorFeaturesEXT border{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        bool portable=false;for(const auto& e:extensions)if(!strcmp(e.extensionName,VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))portable=true;
        VkPhysicalDevicePortabilitySubsetFeaturesKHR portability_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR};
        if(has_border){features.pNext=&border;if(portable)border.pNext=&portability_features;}
        else if(portable)features.pNext=&portability_features;
        vkGetPhysicalDeviceFeatures2(gpu,&features);di.pNext=features.pNext;
        std::vector<const char*> enabled_extensions;
        if(has_border){di.pNext=&border;enabled_extensions.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);}
        for(const auto& e:extensions)if(!strcmp(e.extensionName,"VK_KHR_portability_subset"))
            enabled_extensions.push_back("VK_KHR_portability_subset");
        di.enabledExtensionCount=enabled_extensions.size();di.ppEnabledExtensionNames=enabled_extensions.data();
        vkcheck(vkCreateDevice(gpu, &di, nullptr, &device));
        volkLoadDevice(device);mng_vulkan_identify(gpu);
        VkQueue queue{};
        vkGetDeviceQueue(device, family, 0, &queue);
        xbg_game_config config{};
        config.size = sizeof(config);
        config.physical = gpu;
        config.device = device;
        config.queue = queue;
        config.queue_family = family;
        config.fence_timeout_ns = 5000000000ull;
        config.shader_directory = synthetic_directory.c_str();
        config.enabled_core=enabled;
        config.present=present_probe_submit;
        const char *old_bc=getenv("DM_GFX_NATIVE_BC");
        const std::string old_bc_value=old_bc?old_bc:"";
        setenv("DM_GFX_NATIVE_BC",enabled.textureCompressionBC?"1":"0",1);
        char error[512]{};
        need(mng_nvn_initialize(&config, error, sizeof(error)) == 0, error);
        if(old_bc)setenv("DM_GFX_NATIVE_BC",old_bc_value.c_str(),1);else unsetenv("DM_GFX_NATIVE_BC");
        initialized = true;
        mng_nvn_set_custom_border_color(has_border && border.customBorderColors);
        need(!mng_nvn_get_proc_address("nvnDefinitelyUnsupported"),
             "Unknown API must stay unresolved");
        NVNdeviceBuilder db{};
        NVNdevice nd{};
        NVN(nvnDeviceBuilderSetDefaults)(&db);
        need(NVN(nvnDeviceInitialize)(&nd, &db), "Device initialization");
        command_accounting_test(&nd);
        concurrent_command_test(&nd);
        transient_builder_test(&nd);
        constexpr size_t bytes = 4u << 20;
        need(posix_memalign(&storage, 65536, bytes) == 0, "Pool allocation");
        memset(storage, 0, bytes);
        NVNmemoryPoolBuilder pb{};
        NVNmemoryPool pool{};
        NVN(nvnMemoryPoolBuilderSetDefaults)(&pb);
        NVN(nvnMemoryPoolBuilderSetDevice)(&pb, &nd);
        NVN(nvnMemoryPoolBuilderSetStorage)(&pb, storage, bytes);
        NVN(nvnMemoryPoolBuilderSetFlags)
        (&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
        need(NVN(nvnMemoryPoolInitialize)(&pool, &pb), "Pool initialization");
        NVNbufferBuilder oversized_builder{};NVNbuffer oversized{};
        NVN(nvnBufferBuilderSetDefaults)(&oversized_builder);NVN(nvnBufferBuilderSetDevice)(&oversized_builder,&nd);
        NVN(nvnBufferBuilderSetStorage)(&oversized_builder,&pool,0,bytes*2);
        need(NVN(nvnBufferInitialize)(&oversized,&oversized_builder),"Oversized buffer descriptor initialize");
        need(NVN(nvnBufferGetSize)(&oversized)==bytes*2 && NVN(nvnBufferMap)(&oversized)==storage,
             "Buffer descriptor must retain requested size and backed base");
        NVN(nvnBufferFlushMappedRange)(&oversized,0,bytes*2);
        NVN(nvnBufferInvalidateMappedRange)(&oversized,0,bytes*2);
        for(size_t i=0;i<bytes;++i)need(static_cast<uint8_t*>(storage)[i]==0,"Cache maintenance modified pool bytes");
        NVN(nvnBufferFinalize)(&oversized);
        NVN(nvnBufferBuilderSetStorage)(&oversized_builder,&pool,0,SIZE_MAX);
        need(!NVN(nvnBufferInitialize)(&oversized,&oversized_builder),"Overflowing buffer descriptor accepted");
        NVNtextureBuilder tb{};
        alignas(8) uint8_t texture_storage[144]{};
        memset(texture_storage + 128, 0xa5, sizeof(texture_storage) - 128);
        auto &texture = *reinterpret_cast<NVNtexture *>(texture_storage);
        NVN(nvnTextureBuilderSetDefaults)(&tb);
        NVN(nvnTextureBuilderSetDevice)(&tb, &nd);
        NVN(nvnTextureBuilderSetSize2D)(&tb, 64, 32);
        NVN(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
        NVN(nvnTextureBuilderSetStorage)(&tb, &pool, 0);
        need(NVN(nvnTextureBuilderGetStorageSize)(&tb) < 65536, "Texture storage bounds");
        value_lifetime_test(&nd,&tb);
        need(NVN(nvnTextureInitialize)(&texture, &tb), "Texture initialization");
        need(NVN(nvnTextureBuilderGetZCullStorageSize)(&tb)==0 &&
             NVN(nvnTextureGetZCullStorageSize)(&texture)==0,"Host-owned ZCull metadata");
        for (size_t i = 128; i < sizeof(texture_storage); ++i)
            need(texture_storage[i] == 0xa5, "Opaque texture overwrote guest storage");
        alignas(8) uint8_t texture_copy[128];
        memcpy(texture_copy, texture_storage, sizeof texture_copy);
        auto *copied_texture=reinterpret_cast<NVNtexture*>(texture_copy);
        NVN(nvnTextureFinalize)(copied_texture);
        // The original remains live after finalizing a by-value copy; later
        // GPU clears/draws exercise its descriptor, not only the size getter.
        need(NVN(nvnTextureGetWidth)(&texture)==64,"Finalized copy changed original texture");
        memcpy(texture_copy, texture_storage, sizeof texture_copy);
        NVN(nvnTextureBuilderSetSize2D)(&tb,32,16);
        need(NVN(nvnTextureInitialize)(copied_texture,&tb),"Copied texture reinitialization");
        need(NVN(nvnTextureGetWidth)(&texture)==64 && NVN(nvnTextureGetWidth)(copied_texture)==32,
             "Reinitialization changed copied texture identity");
        NVN(nvnTextureFinalize)(copied_texture);
        alignas(8) uint8_t view_storage[32]{};
        memset(view_storage + 16, 0xa5, 16);
        auto *view = reinterpret_cast<NVNtextureView *>(view_storage);
        NVN(nvnTextureViewSetDefaults)(view);
        NVN(nvnTextureViewSetLevels)(view, 0, 1);
        NVN(nvnTextureViewSetLayers)(view, 0, 1);
        for (size_t i = 16; i < 32; ++i)
            need(view_storage[i] == 0xa5, "Texture view exceeded 16 bytes");
        alignas(8) uint8_t view_copy[16];
        memcpy(view_copy, view_storage, 16);
        view = reinterpret_cast<NVNtextureView *>(view_copy);

        NVNqueueBuilder qb{};
        NVNqueue nq{};
        NVN(nvnQueueBuilderSetDefaults)(&qb);
        NVN(nvnQueueBuilderSetDevice)(&qb, &nd);
        need(NVN(nvnQueueInitialize)(&nq, &qb), "Queue initialization");
        NVNcommandBuffer cb{};
        need(NVN(nvnCommandBufferInitialize)(&cb, &nd), "Command initialization");
        NVN(nvnCommandBufferAddCommandMemory)(&cb, &pool, 65536, 65536);
        NVN(nvnCommandBufferAddCommandMemory)(&cb, nullptr, 0, 0);
        need(NVN(nvnCommandBufferGetCommandMemorySize)(&cb) == 0 &&
             NVN(nvnCommandBufferGetCommandMemoryUsed)(&cb) == 0,
             "Empty command memory resets accounting");
        NVN(nvnCommandBufferAddCommandMemory)(&cb, &pool, 65536, 65536);
        NVN(nvnCommandBufferAddCommandMemory)(&cb, nullptr, 0, 512);
        need(NVN(nvnCommandBufferGetCommandMemorySize)(&cb) == 512,
             "Null command pool retains declared capacity without mapping it");
        need(!mng_nvn_test_registry_progress(&pool),"Pool getters or command recording blocked behind translation");
        need(!mng_nvn_test_invalidate_ordering(&pool),"Range Invalidate lost output/alias/ordered policy or independent progress");
        fprintf(stderr,"[probe] Invalidate OFF/ON: empty/image-only/disjoint/retired progress; writeback/report/event/unknown/reuse/alias/ordered/full-policy blocked\n");
        need(!mng_nvn_test_flush_ordering(&pool),"Potential GPU output bypassed conservative flush ordering");
        fprintf(stderr,"[probe] unrelated/retired mapped flush progresses; live GPU output retains translation ordering\n");
        fprintf(stderr,"[probe] object getters and complete command recordings progress while translation owns the frontend lock\n");
        alignas(8) uint8_t control_memory[128]{};
        // Exercise the actual Vulkan report path against the guest's CPU clock.
        // Raw device uptime previously appeared days in the future on Odin3.
        {
            // Match Guest GetSystemTick. Darwin steady_clock includes system
            // sleep; the guest and GPU report conversion use mach_absolute_time.
            auto now=[] {return mng_host_clock_ns(0);};
            auto *reports=reinterpret_cast<NVNcounterData*>(static_cast<uint8_t*>(storage)+(3u<<20));
            uint64_t previous{};
            for(unsigned iteration=0;iteration<3;++iteration){
                const auto before=now();
                NVN(nvnCommandBufferAddControlMemory)(&cb,control_memory,sizeof control_memory);
                NVN(nvnCommandBufferBeginRecording)(&cb);
                NVN(nvnCommandBufferReportCounter)(&cb,NVN_COUNTER_TYPE_TIMESTAMP_TOP,uintptr_t(&reports[0]));
                NVN(nvnCommandBufferReportCounter)(&cb,NVN_COUNTER_TYPE_TIMESTAMP,uintptr_t(&reports[1]));
                auto commands=NVN(nvnCommandBufferEndRecording)(&cb);
                memset(reports,0xff,2*sizeof *reports);OrderedProbeGate gate;
                const auto accepted=ordered_stat("ordered_report_submits");
                NVN(nvnQueueSubmitCommands)(&nq,1,&commands);gate.check();
                if(gate.active){
                    need(ordered_stat("ordered_report_submits")==accepted+1,"Report test ran synchronously");
                    need(reports[0].timestamp==UINT64_MAX && reports[1].timestamp==UINT64_MAX,"Report visible before translation");
                    if(iteration==0)ordered_fence_test(&nd,&nq,gate);
                    else if(iteration==1){
                        auto invalidate=std::async(std::launch::async,[&]{NVN(nvnMemoryPoolInvalidateMappedRange)(&pool,3u<<20,2*sizeof *reports);});
                        need(invalidate.wait_for(std::chrono::milliseconds(10))==std::future_status::timeout,"Invalidate bypassed translation");
                        gate.unblock();invalidate.get();
                        need(reports[0].timestamp!=UINT64_MAX && reports[1].timestamp!=UINT64_MAX,"Invalidate missed report writeback");
                    }else gate.unblock();
                }
                NVN(nvnQueueFlush)(&nq);
                NVN(nvnMemoryPoolInvalidateMappedRange)(&pool,3u<<20,2*sizeof *reports);
                need(reports[0].timestamp!=UINT64_MAX && reports[1].timestamp!=UINT64_MAX,"Invalidate missed submitted report before QueueFinish");
                NVN(nvnQueueFinish)(&nq);
                const auto after=now();
                const auto top=NVN(nvnDeviceGetTimestampInNanoseconds)(&nd,&reports[0]);
                const auto bottom=NVN(nvnDeviceGetTimestampInNanoseconds)(&nd,&reports[1]);
                need(top+10000000>=before && bottom<=after+10000000,"GPU reports use a different epoch from Guest GetSystemTick");
                need(bottom>=top && top>=previous,"GPU query ordering changed by clock conversion");
                previous=bottom;std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            fprintf(stderr,"[probe] GPU report clock agrees with Guest monotonic clock\n");
        }
        counter_pool_handoff_test(&nd,&nq,&cb);
        {
            const auto before=ordered_stat("ordered_fallback_9");
            NVN(nvnCommandBufferAddControlMemory)(&cb,control_memory,sizeof control_memory);
            NVN(nvnCommandBufferBeginRecording)(&cb);
            const NVNtexture *targets[]={&texture};const NVNtextureView *views[]={view};
            NVN(nvnCommandBufferSetRenderTargets)(&cb,1,targets,views,nullptr,nullptr);
            NVN(nvnCommandBufferReportCounter)(&cb,NVN_COUNTER_TYPE_TIMESTAMP,uintptr_t(storage));
            const float color[]={0,0,0,1};
            NVN(nvnCommandBufferClearColor)(&cb,0,color,NVN_CLEAR_COLOR_MASK_RGBA);
            auto handle=NVN(nvnCommandBufferEndRecording)(&cb);
            NVN(nvnQueueSubmitCommands)(&nq,1,&handle);NVN(nvnQueueFinish)(&nq);
            if(ordered_stat("ordered_translation"))need(ordered_stat("ordered_fallback_9")==before+1,"Report/input alias did not fall back");
            fprintf(stderr,"[probe] report/attachment input alias retains serial ordering\n");
        }
        xbg_game_stats clock_baseline{};xbg_game_get_stats(&clock_baseline);
        auto record_clear = [&](int x, int y, int width, int height, const float *color) {
            NVN(nvnCommandBufferAddControlMemory)(&cb,control_memory,sizeof(control_memory));
            NVN(nvnCommandBufferBeginRecording)(&cb);
            const NVNtexture *targets[] = {&texture};
            const NVNtextureView *views[] = {view};
            NVN(nvnCommandBufferSetRenderTargets)(&cb, 1, targets, views, nullptr, nullptr);
            NVN(nvnCommandBufferSetTiledCacheTileSize)(&cb,64,32);
            NVN(nvnCommandBufferSetTiledCacheAction)(&cb,NVN_TILED_CACHE_ACTION_ENABLE);
            NVN(nvnCommandBufferSetViewport)(&cb, 0, 0, 64, 32);
            NVN(nvnCommandBufferSetScissor)(&cb, x, y, width, height);
            NVN(nvnCommandBufferClearColor)(&cb, 0, color, 15);
            NVN(nvnCommandBufferSetTiledCacheAction)(&cb,NVN_TILED_CACHE_ACTION_ENABLE_RENDER_TARGET_BINNING);
            NVN(nvnCommandBufferSetTiledCacheAction)(&cb,NVN_TILED_CACHE_ACTION_FLUSH);
            NVN(nvnCommandBufferSetTiledCacheAction)(&cb,NVN_TILED_CACHE_ACTION_FLUSH_NO_TILING);
            NVN(nvnCommandBufferSetTiledCacheAction)(&cb,NVN_TILED_CACHE_ACTION_DISABLE_RENDER_TARGET_BINNING);
            NVN(nvnCommandBufferSetTiledCacheAction)(&cb,NVN_TILED_CACHE_ACTION_DISABLE);
            return NVN(nvnCommandBufferEndRecording)(&cb);
        };
        float background[] = {0.25f, 0.5f, 0.75f, 1};
        auto first = record_clear(0, 0, 64, 32, background);
        NVN(nvnQueueSubmitCommands)(&nq, 1, &first);
        NVNsync sync{};
        need(NVN(nvnSyncInitialize)(&sync, &nd), "Sync initialize");
        need(NVN(nvnSyncWait)(&sync, 0) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED,
             "Unfenced sync must not signal");
        NVN(nvnQueueFenceSync)(&nq, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 1);
        NVN(nvnQueueFlush)(&nq);
        auto wait = NVN(nvnSyncWait)(&sync, 5000000000ull);
        need(wait == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED ||
                 wait == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED,
             "GPU fence completion");
        NVNcopyRegion region{0, 0, 0, 64, 32, 1};
        std::vector<uint8_t> readback(64 * 32 * 4);
        NVN(nvnTextureReadTexels)(&texture, nullptr, &region, readback.data());
        for (size_t i = 0; i < readback.size(); i += 4)
            need(readback[i] == 64 && readback[i + 1] == 128 && readback[i + 2] == 191 &&
                     readback[i + 3] == 255,
                 "First GPU clear readback mismatch");
        // GPU contents belong to storage, not one opaque by-value copy. This
        // regression reproduces a full black source after finalizing a copy.
        NVNtexture rendered_copy{};memcpy(&rendered_copy,&texture,128);
        NVN(nvnTextureFinalize)(&rendered_copy);
        NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
        for(size_t i=0;i<readback.size();i+=4)
            need(readback[i]==64 && readback[i+1]==128 && readback[i+2]==191 && readback[i+3]==255,
                 "Finalizing a copied texture discarded live GPU contents");
        // New NVN identities with identical storage must reuse the GPU image,
        // including after another alias is finalized. CPU backing still holds
        // pre-clear pixels, so rebuilding/uploading an alias is observable.
        NVNtexture alias{};
        NVN(nvnTextureBuilderSetSize2D)(&tb,64,32);
        for (unsigned i = 0; i < 256; ++i) {
            need(NVN(nvnTextureInitialize)(&alias, &tb), "Image alias initialize");
            NVN(nvnCommandBufferBeginRecording)(&cb);
            const NVNtexture *target=&alias;
            NVN(nvnCommandBufferSetRenderTargets)(&cb,1,&target,nullptr,nullptr,nullptr);
            NVN(nvnCommandBufferClearColor)(&cb,0,background,8);
            auto command=NVN(nvnCommandBufferEndRecording)(&cb);
            NVN(nvnQueueSubmitCommands)(&nq,1,&command);
            {
                FinalizeWaitAudit audit;
                NVN(nvnTextureFinalize)(&alias);
                need(!FinalizeWaitAudit::calls,"Texture finalization drained the GPU queue");
            }
            // The submitted clear and original by-value identity must survive
            // finalization before the explicit completion/readback boundary.
            NVN(nvnQueueFinish)(&nq);
            NVN(nvnTextureReadTexels)(&texture, nullptr, &region, readback.data());
            for (size_t pixel = 0; pixel < readback.size(); pixel += 4)
                need(readback[pixel] == 64 && readback[pixel + 1] == 128 &&
                         readback[pixel + 2] == 191 && readback[pixel + 3] == 255,
                     "Image alias lost GPU contents");
        }
        NVN(nvnTextureBuilderSetSize2D)(&tb,32,16);
        float red[] = {1, 0, 0, 1};
        auto source = record_clear(8, 4, 16, 8, red);
        NVNcommandBuffer copy{};
        need(NVN(nvnCommandBufferInitialize)(&copy, &nd), "Copy command initialize");
        NVN(nvnCommandBufferBeginRecording)(&copy);
        NVN(nvnCommandBufferSaveZCullData)(&copy,0,0);
        NVN(nvnCommandBufferRestoreZCullData)(&copy,0,0);
        NVN(nvnCommandBufferCopyCommands)(&copy, 1, &source);
        auto second = NVN(nvnCommandBufferEndRecording)(&copy);
        // Both a stable control-memory handle and the shared command region
        // are recycled before submission. The caller must retain the red clear.
        float blue[]={0,0,1,1};
        auto overwritten=record_clear(0,0,64,32,blue);
        need(source==overwritten,"Control-memory handle must name latest recording");
        need(NVN(nvnCommandBufferGetControlMemoryUsed)(&cb)==64,"Control-memory accounting");
        // Recycle transient render-target recordings beyond the old 65,536
        // process-wide snapshot limit. The earlier inlined red clear must stay
        // immutable while the source control slot and new textures are reused.
        NVNcommandBuffer churn{};NVNtexture transient{};
        alignas(8) uint8_t churn_control[64]{};
        need(NVN(nvnCommandBufferInitialize)(&churn,&nd),"Texture churn command initialize");
        NVNcommandHandle churn_handle{};
        for(unsigned i=0;i<66000;++i){
            need(NVN(nvnTextureInitialize)(&transient,&tb),"Texture churn initialize");
            NVN(nvnCommandBufferAddControlMemory)(&churn,churn_control,sizeof churn_control);
            NVN(nvnCommandBufferBeginRecording)(&churn);
            const NVNtexture *target=&transient;
            NVN(nvnCommandBufferSetRenderTargets)(&churn,1,&target,nullptr,nullptr,nullptr);
            churn_handle=NVN(nvnCommandBufferEndRecording)(&churn);
        }
        NVN(nvnDeviceFinalizeCommandHandle)(&nd,churn_handle);
        NVN(nvnCommandBufferFinalize)(&churn);
        NVN(nvnTextureFinalize)(&transient);
        NVN(nvnQueueSubmitCommands)(&nq, 1, &second);
        NVN(nvnQueueFinish)(&nq);
        NVN(nvnTextureReadTexels)(&texture, nullptr, &region, readback.data());
        unsigned red_pixels = 0;
        for (size_t i = 0; i < readback.size(); i += 4) {
            bool is_red = readback[i] == 255 && readback[i + 1] == 0 && readback[i + 2] == 0 &&
                          readback[i + 3] == 255;
            if (is_red)
                ++red_pixels;
            else
                need(readback[i] == 64 && readback[i + 1] == 128 && readback[i + 2] == 191 &&
                         readback[i + 3] == 255,
                     "Scissor corrupted background");
        }
        need(red_pixels == 128, "Scissor clear pixel count");
        // These are handwritten test shaders, never extracted game shaders.
        // Exercise offline SPIR-V loading and a real graphics pipeline/draw.
        NVNprogram program{};
        need(NVN(nvnProgramInitialize)(&program, &nd), "Program initialize");
        NVNshaderData shaders[] = {{0, controls[0].data()}, {0, controls[1].data()}};
        need(NVN(nvnProgramSetShaders)(&program, 2, shaders), "Offline shader assignment");
        NVN(nvnCommandBufferBeginRecording)(&cb);
        NVN(nvnCommandBufferSetScissor)(&cb, 0, 0, 64, 32);
        NVN(nvnCommandBufferSetLineWidth)(&cb,1.0f);
        NVN(nvnCommandBufferSetPointSize)(&cb,1.0f);
        NVN(nvnCommandBufferBindProgram)
        (&cb, &program, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
        auto *indices=reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(storage)+(1u<<20));
        indices[0]=1;indices[1]=2;indices[2]=3;
        NVN(nvnMemoryPoolFlushMappedRange)(&pool,1u<<20,6);
        NVN(nvnCommandBufferDrawElementsBaseVertex)
        (&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,NVN_INDEX_TYPE_UNSIGNED_SHORT,3,uintptr_t(indices),-1);
        auto third = NVN(nvnCommandBufferEndRecording)(&cb);
        NVN(nvnQueueSubmitCommands)(&nq, 1, &third);
        NVN(nvnQueueFinish)(&nq);
        NVN(nvnTextureReadTexels)(&texture, nullptr, &region, readback.data());
        for (size_t i = 0; i < readback.size(); i += 4)
            need(readback[i] == 0 && readback[i + 1] == 255 && readback[i + 2] == 0 &&
                     readback[i + 3] == 255,
                 "Offline shader GPU draw readback mismatch");
        NVN(nvnProgramFinalize)(&program);
        NVNprogram compute{};
        need(NVN(nvnProgramInitialize)(&compute, &nd), "Compute program initialize");
        NVNshaderData compute_shader{0, controls[2].data()};
        need(NVN(nvnProgramSetShaders)(&compute, 1, &compute_shader), "Compute shader assignment");
        auto *output = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(storage) + (2u << 20));
        memset(output, 0, 256);
        NVN(nvnCommandBufferBeginRecording)(&cb);
        NVN(nvnCommandBufferBindProgram)(&cb, &compute, NVN_SHADER_STAGE_COMPUTE_BIT);
        NVN(nvnCommandBufferBindStorageBuffer)
        (&cb, NVN_SHADER_STAGE_COMPUTE, 0, uintptr_t(output), 256);
        NVN(nvnCommandBufferDispatchCompute)(&cb, 64, 1, 1);
        auto fourth = NVN(nvnCommandBufferEndRecording)(&cb);
        NVN(nvnQueueSubmitCommands)(&nq, 1, &fourth);
        NVN(nvnQueueFinish)(&nq);
        NVN(nvnMemoryPoolInvalidateMappedRange)(&pool, 2u << 20, 256);
        for (unsigned i = 0; i < 64; ++i)
            need(output[i] == i * 17 + 3, "Compute storage writeback mismatch");
        xbg_game_stats stats{};
        xbg_game_get_stats(&stats);
        need(stats.submissions-clock_baseline.submissions == 260 && stats.shader_loads == 3 && stats.draws == 1 &&
                 stats.dispatches == 1,
             "Submission count");
        // The guest command APIs share the engine's pitched staging path and
        // own immutable texture metadata through command-handle retirement.
        constexpr size_t copy_pitch=272,copy_bytes=copy_pitch*32;
        auto* copy_memory=static_cast<uint8_t*>(storage)+(3u<<20);
        memset(copy_memory,0xa5,copy_bytes);
        NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
        NVN(nvnCommandBufferBeginRecording)(&cb);
        NVN(nvnCommandBufferSetCopyRowStride)(&cb,copy_pitch);
        NVN(nvnCommandBufferSetCopyImageStride)(&cb,copy_bytes+16);
        NVN(nvnCommandBufferCopyTextureToBuffer)(&cb,&texture,nullptr,&region,uintptr_t(copy_memory),0);
        auto copy_handle=NVN(nvnCommandBufferEndRecording)(&cb);
        NVN(nvnQueueSubmitCommands)(&nq,1,&copy_handle);NVN(nvnQueueFlush)(&nq);
        NVN(nvnMemoryPoolInvalidateMappedRange)(&pool,3u<<20,copy_bytes);
        for(unsigned y=0;y<32;++y){
            need(!memcmp(copy_memory+y*copy_pitch,readback.data()+y*256,256),"Command texture readback pitch mismatch");
            for(unsigned x=256;x<copy_pitch;++x)need(copy_memory[y*copy_pitch+x]==0xa5,"Texture readback changed row padding");
        }
        NVN(nvnQueueFinish)(&nq);
        copy_memory[0]=201;readback[0]=201;
        NVN(nvnMemoryPoolFlushMappedRange)(&pool,3u<<20,copy_bytes);
        NVN(nvnCommandBufferBeginRecording)(&cb);
        NVN(nvnCommandBufferCopyBufferToTexture)(&cb,uintptr_t(copy_memory),&texture,nullptr,&region,0);
        NVN(nvnCommandBufferSetCopyRowStride)(&cb,0);NVN(nvnCommandBufferSetCopyImageStride)(&cb,0);
        copy_handle=NVN(nvnCommandBufferEndRecording)(&cb);
        NVN(nvnQueueSubmitCommands)(&nq,1,&copy_handle);NVN(nvnQueueFinish)(&nq);
        std::vector<uint8_t> copied_pixels(readback.size());
        NVN(nvnTextureReadTexels)(&texture,nullptr,&region,copied_pixels.data());
        need(copied_pixels==readback,"Pitched buffer-to-texture command roundtrip mismatch");
        // GPU-to-GPU chains must stay resident. Partial overlaps (including a
        // newer partial write over an older full write) must resolve before
        // descriptors/commands are acquired, and preserve every output word.
        NVNprogram increment{};
        need(NVN(nvnProgramInitialize)(&increment,&nd),"Increment program initialize");
        NVNshaderData increment_shader{0,controls[6].data()};
        need(NVN(nvnProgramSetShaders)(&increment,1,&increment_shader),"Increment shader assignment");
        for(unsigned mode=0;mode<4;++mode){
            memset(output,0,512);
            NVN(nvnCommandBufferBeginRecording)(&cb);
            NVN(nvnCommandBufferBindProgram)(&cb,&compute,NVN_SHADER_STAGE_COMPUTE_BIT);
            NVN(nvnCommandBufferBindStorageBuffer)(&cb,NVN_SHADER_STAGE_COMPUTE,0,uintptr_t(output),256);
            NVN(nvnCommandBufferDispatchCompute)(&cb,64,1,1);
            // An unrelated image clear must not drain this storage result.
            const NVNtexture *batch_target=&texture;
            NVN(nvnCommandBufferSetRenderTargets)(&cb,1,&batch_target,nullptr,nullptr,nullptr);
            NVN(nvnCommandBufferClearColor)(&cb,0,blue,15);
            // An empty dispatch does not consume its unbound resources.
            NVN(nvnCommandBufferBindStorageBuffer)(&cb,NVN_SHADER_STAGE_COMPUTE,0,0,0);
            NVN(nvnCommandBufferDispatchCompute)(&cb,0,1,1);
            NVN(nvnCommandBufferBindProgram)(&cb,&increment,NVN_SHADER_STAGE_COMPUTE_BIT);
            const unsigned offset=mode==0?64:mode==2?32:0;
            NVN(nvnCommandBufferBindStorageBuffer)(&cb,NVN_SHADER_STAGE_COMPUTE,0,uintptr_t(output+offset),mode==3?128:256);
            NVN(nvnCommandBufferDispatchCompute)(&cb,mode==3?32:64,1,1);
            NVN(nvnCommandBufferSetRenderTargets)(&cb,1,&batch_target,nullptr,nullptr,nullptr);
            NVN(nvnCommandBufferClearColor)(&cb,0,blue,15);
            if(mode==3){
                NVN(nvnCommandBufferBindStorageBuffer)(&cb,NVN_SHADER_STAGE_COMPUTE,0,uintptr_t(output),256);
                NVN(nvnCommandBufferDispatchCompute)(&cb,64,1,1);
            }
            auto commands=NVN(nvnCommandBufferEndRecording)(&cb);
            const auto before=mng_nvn_range_preflight_resolves();
            NVN(nvnQueueSubmitCommands)(&nq,1,&commands);
            const auto resolves=mng_nvn_range_preflight_resolves()-before;
            const char *ranged=getenv("MNG_RANGE_PREFLIGHT");
            const char *batch=getenv("MNG_SUBMIT_BATCH_OPS");
            if(!ranged || strcmp(ranged,"0")){
                if(batch && !strcmp(batch,"1"))need(resolves<=(mode>=2?1u:0u),"Batched read preflight synchronized an independent/GPU view");
                else need(resolves==(mode>=2?1u:0u),"Read preflight synchronized an independent/GPU view or missed a partial overlap");
            }
            NVN(nvnQueueFlush)(&nq);
            NVN(nvnMemoryPoolInvalidateMappedRange)(&pool,reinterpret_cast<uintptr_t>(output)-reinterpret_cast<uintptr_t>(storage),512);
            for(unsigned i=0;i<128;++i){
                uint32_t expected=i<64?i*17+3:0;
                if(i>=offset && i<offset+(mode==3?32:64))++expected;
                if(mode==3 && i<64)++expected;
                need(output[i]==expected,"GPU writeback chain/partial overlap mismatch");
            }
            NVN(nvnQueueFinish)(&nq);
        }
        {
            constexpr size_t offset=(3u<<20)+16384;
            auto* result=reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(storage)+offset);
            for(unsigned value:{17u,29u}){
                *result=0;
                NVN(nvnCommandBufferBeginRecording)(&cb);
                // Keep real GPU work before the event so defer-events probes
                // exercise pending/recording event leases, not only immediate writes.
                const NVNtexture* target=&texture;
                NVN(nvnCommandBufferSetRenderTargets)(&cb,1,&target,nullptr,nullptr,nullptr);
                NVN(nvnCommandBufferClearColor)(&cb,0,blue,15);
                mng_nvn_test_record_event(&cb,uintptr_t(result),value);
                auto commands=NVN(nvnCommandBufferEndRecording)(&cb);
                NVN(nvnQueueSubmitCommands)(&nq,1,&commands);NVN(nvnQueueFlush)(&nq);
                NVN(nvnMemoryPoolInvalidateMappedRange)(&pool,offset,4);
                need(*result==value,"Invalidate missed submitted event before QueueFinish");
                NVN(nvnQueueFinish)(&nq);
            }
            fprintf(stderr,"[probe] range Invalidate observed GPU reports, pitched copies, storage chains and bottom events before QueueFinish\n");
        }
        NVN(nvnProgramFinalize)(&compute);NVN(nvnProgramFinalize)(&increment);
        fprintf(stderr,"[probe] 4 GPU writeback chains: 512 words, independent/full/partial/newest-partial preflight verified\n");
        if(const char *batch=getenv("MNG_SUBMIT_BATCH_OPS");batch && !strcmp(batch,"1"))
        {
            const char* adaptive=getenv("MNG_ADAPTIVE_BATCH");
            if(adaptive && !strcmp(adaptive,"0"))
                need(mng_nvn_batch_submissions()>=8,"Cross-slot writeback batch coverage missing");
            else need(mng_nvn_batch_submissions()+mng_nvn_batch_deferred()>=8,"Adaptive batch admission coverage missing");
            fprintf(stderr,"[probe] batch admission: submitted=%llu deferred=%llu; GPU writeback pixels verified\n",
                (unsigned long long)mng_nvn_batch_submissions(),(unsigned long long)mng_nvn_batch_deferred());
        }
        // BOTW writes eight slices of a volume through separate color views.
        // Check every slice so a view accidentally targeting slice zero fails.
        NVNtexture volume{};
        NVN(nvnTextureBuilderSetDefaults)(&tb);
        NVN(nvnTextureBuilderSetDevice)(&tb, &nd);
        NVN(nvnTextureBuilderSetTarget)(&tb, NVN_TEXTURE_TARGET_3D);
        NVN(nvnTextureBuilderSetSize3D)(&tb, 8, 8, 8);
        NVN(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
        NVN(nvnTextureBuilderSetStorage)(&tb, &pool, 3u << 20);
        need(NVN(nvnTextureInitialize)(&volume, &tb), "Volume texture initialize");
        NVNtextureView slices[8]{};
        const NVNtexture *slice_targets[8];
        const NVNtextureView *slice_views[8];
        for (unsigned i = 0; i < 8; ++i) {
            NVN(nvnTextureViewSetDefaults)(&slices[i]);
            NVN(nvnTextureViewSetLevels)(&slices[i], 0, 0);
            NVN(nvnTextureViewSetLayers)(&slices[i], i, i==7?0:1);
            int base=-1,count=-1;
            need(NVN(nvnTextureViewGetLevels)(&slices[i],&base,&count) && base==0 && count==0,
                 "View getters must retain open-ended mip count");
            slice_targets[i] = &volume; slice_views[i] = &slices[i];
        }
        NVN(nvnCommandBufferBeginRecording)(&cb);
        NVN(nvnCommandBufferSetRenderTargets)(&cb, 8, slice_targets, slice_views, nullptr, nullptr);
        NVN(nvnCommandBufferSetViewport)(&cb, 0, 0, 8, 8);
        NVN(nvnCommandBufferSetScissor)(&cb, 0, 0, 8, 8);
        for (unsigned i = 0; i < 8; ++i) {
            const float color[] = {float((i & 1) != 0), float((i & 2) != 0), float((i & 4) != 0), 1};
            NVN(nvnCommandBufferClearColor)(&cb, i, color, 15);
        }
        auto volume_commands = NVN(nvnCommandBufferEndRecording)(&cb);
        NVN(nvnQueueSubmitCommands)(&nq, 1, &volume_commands);
        NVN(nvnQueueFinish)(&nq);
        NVNcopyRegion volume_region{0, 0, 0, 8, 8, 8};
        std::array<uint8_t, 8 * 8 * 8 * 4> volume_pixels{};
        NVN(nvnTextureReadTexels)(&volume, nullptr, &volume_region, volume_pixels.data());
        for (unsigned slice = 0; slice < 8; ++slice)
            for (unsigned pixel = 0; pixel < 64; ++pixel) {
                const auto *rgba = volume_pixels.data() + (slice * 64 + pixel) * 4;
                need(rgba[0] == ((slice & 1) ? 255 : 0) &&
                     rgba[1] == ((slice & 2) ? 255 : 0) &&
                     rgba[2] == ((slice & 4) ? 255 : 0) && rgba[3] == 255,
                     "Volume attachment slice readback mismatch");
            }
        NVN(nvnTextureFinalize)(&volume);
        unsigned block_words=0;
        if(enabled.textureCompressionBC){
            NVNtexture blocks{};NVNtextureView raw{};
            NVN(nvnTextureBuilderSetDefaults)(&tb);
            NVN(nvnTextureBuilderSetDevice)(&tb,&nd);
            NVN(nvnTextureBuilderSetTarget)(&tb,NVN_TEXTURE_TARGET_2D);
            NVN(nvnTextureBuilderSetSize2D)(&tb,8,8);
            NVN(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGTC1_UNORM);
            NVN(nvnTextureBuilderSetStorage)(&tb,&pool,3u<<20);
            need(NVN(nvnTextureInitialize)(&blocks,&tb),"BC block texture initialize");
            NVN(nvnTextureViewSetDefaults)(&raw);
            NVN(nvnTextureViewSetFormat)(&raw,NVN_FORMAT_RGBA16UI);
            NVN(nvnTextureViewSetLevels)(&raw,0,1);
            const NVNtexture *target=&blocks;const NVNtextureView *view=&raw;
            NVN(nvnCommandBufferBeginRecording)(&cb);
            NVN(nvnCommandBufferSetRenderTargets)(&cb,1,&target,&view,nullptr,nullptr);
            NVN(nvnCommandBufferSetViewport)(&cb,0,0,2,2);
            NVN(nvnCommandBufferSetScissor)(&cb,0,0,2,2);
            const float raw_color[]{4660,22136,39612,57072};
            NVN(nvnCommandBufferClearColor)(&cb,0,raw_color,15);
            // Force a render attachment path, not only a whole-image transfer
            // clear: raw block views must carry Metal RenderTarget usage too.
            const float clipped_color[]{33,44,55,66};
            NVN(nvnCommandBufferSetScissor)(&cb,0,0,1,2);
            NVN(nvnCommandBufferClearColor)(&cb,0,clipped_color,15);
            auto command=NVN(nvnCommandBufferEndRecording)(&cb);
            NVN(nvnQueueSubmitCommands)(&nq,1,&command);NVN(nvnQueueFinish)(&nq);
            std::array<uint16_t,16> result{};NVNcopyRegion region{0,0,0,8,8,1};
            NVN(nvnTextureReadTexels)(&blocks,nullptr,&region,result.data());
            for(unsigned i=0;i<result.size();++i)need(result[i]==uint16_t(((i/4)%2==0?clipped_color:raw_color)[i%4]),"BC integer block attachment readback mismatch");
            block_words=result.size();NVN(nvnTextureFinalize)(&blocks);

            // Match BOTW's compressed 32^3 volume written through raw integer
            // depth-slice attachments. A depth slice is not a Vulkan array layer.
            NVN(nvnTextureBuilderSetTarget)(&tb,NVN_TEXTURE_TARGET_3D);
            NVN(nvnTextureBuilderSetSize3D)(&tb,32,32,32);
            need(NVN(nvnTextureInitialize)(&blocks,&tb),"BC volume initialize");
            for(unsigned first=0;first<32;first+=8){
                NVNtextureView views[8]{};
                const NVNtexture *targets[8];const NVNtextureView *view_ptrs[8];
                for(unsigned i=0;i<8;++i){
                    NVN(nvnTextureViewSetDefaults)(&views[i]);
                    NVN(nvnTextureViewSetFormat)(&views[i],NVN_FORMAT_RGBA16UI);
                    NVN(nvnTextureViewSetLevels)(&views[i],0,1);
                    NVN(nvnTextureViewSetLayers)(&views[i],first+i,1);
                    targets[i]=&blocks;view_ptrs[i]=&views[i];
                }
                NVN(nvnCommandBufferBeginRecording)(&cb);
                NVN(nvnCommandBufferSetRenderTargets)(&cb,8,targets,view_ptrs,nullptr,nullptr);
                NVN(nvnCommandBufferSetViewport)(&cb,0,0,8,8);
                NVN(nvnCommandBufferSetScissor)(&cb,0,0,8,8);
                for(unsigned i=0;i<8;++i){
                    const float color[]{float(first+i+1),22,33,44};
                    NVN(nvnCommandBufferClearColor)(&cb,i,color,15);
                }
                NVN(nvnCommandBufferSetScissor)(&cb,0,0,4,8);
                for(unsigned i=0;i<8;++i){
                    const float color[]{float(first+i+101),222,333,444};
                    NVN(nvnCommandBufferClearColor)(&cb,i,color,15);
                }
                auto recorded=NVN(nvnCommandBufferEndRecording)(&cb);
                NVN(nvnQueueSubmitCommands)(&nq,1,&recorded);
                NVN(nvnQueueFinish)(&nq);
            }
            std::array<uint16_t,32*8*8*4> volume_blocks{};
            NVNcopyRegion compressed_region{0,0,0,32,32,32};
            NVN(nvnTextureReadTexels)(&blocks,nullptr,&compressed_region,volume_blocks.data());
            for(unsigned z=0;z<32;++z)for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
                const uint16_t expected[]{uint16_t(z+(x<4?101:1)),uint16_t(x<4?222:22),uint16_t(x<4?333:33),uint16_t(x<4?444:44)};
                for(unsigned c=0;c<4;++c)need(volume_blocks[((z*8+y)*8+x)*4+c]==expected[c],"BC volume slice attachment readback mismatch");
            }
            block_words+=volume_blocks.size();NVN(nvnTextureFinalize)(&blocks);
        }
        unsigned border_pixels=0,swizzle_pixels=0,linear_pixels=0;
        if(true){ // Exercise native custom border or the shared shader fallback.
            NVNtexture sampled{};NVNtexturePool textures{};NVNsamplerPool samplers{};
            NVN(nvnTextureBuilderSetDefaults)(&tb);NVN(nvnTextureBuilderSetDevice)(&tb,&nd);
            NVN(nvnTextureBuilderSetSize2D)(&tb,4,4);NVN(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8);
            NVN(nvnTextureBuilderSetStorage)(&tb,&pool,3u<<20);
            need(NVN(nvnTextureInitialize)(&sampled,&tb),"Border test texture initialize");
            need(NVN(nvnTexturePoolInitialize)(&textures,&pool,(2u<<20)+4096,3),"Border test texture pool");
            need(NVN(nvnSamplerPoolInitialize)(&samplers,&pool,(2u<<20)+8192,3),"Border test sampler pool");
            NVNtexture unformatted{};
            NVN(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_NONE);
            need(NVN(nvnTextureInitialize)(&unformatted,&tb),"Unformatted descriptor metadata");
            need(NVN(nvnTextureGetStorageSize)(&unformatted)==0,"Unformatted metadata must not allocate pixels");
            NVN(nvnTexturePoolRegisterTexture)(&textures,1,&unformatted,nullptr);
            NVN(nvnTextureFinalize)(&unformatted);
            NVN(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_RGBA8);
            // Descriptor registration owns a value copy, not a command snapshot.
            // Exercise more identities than the command snapshot arena permits.
            for(unsigned i=0;i<65537;++i){
                NVNtexture temporary{};
                need(NVN(nvnTextureInitialize)(&temporary,&tb),"Descriptor churn texture initialize");
                NVN(nvnTexturePoolRegisterTexture)(&textures,1,&temporary,nullptr);
                NVN(nvnTextureFinalize)(&temporary);
            }
            NVN(nvnTexturePoolRegisterTexture)(&textures,1,&sampled,nullptr);
            NVNprogram border_program{};need(NVN(nvnProgramInitialize)(&border_program,&nd),"Border test program");
            NVNshaderData stages[]{{0,controls[0].data()},{0,controls[7].data()}};
            need(NVN(nvnProgramSetShaders)(&border_program,2,stages),"Border test shaders");
            std::array<uint8_t,64> black_texels{};for(unsigned i=3;i<black_texels.size();i+=4)black_texels[i]=255;
            NVNcopyRegion black_region{0,0,0,4,4,1};
            NVN(nvnTextureWriteTexels)(&sampled,nullptr,&black_region,black_texels.data());
            NVN(nvnTextureFlushTexels)(&sampled,nullptr,&black_region);
            uint64_t first_border_shader_loads=0;
            for(unsigned trial=0;trial<3;++trial){
                NVNsamplerBuilder builder{};NVN(nvnSamplerBuilderSetDefaults)(&builder);
                NVN(nvnSamplerBuilderSetMinMagFilter)(&builder,NVN_MIN_FILTER_LINEAR,NVN_MAG_FILTER_LINEAR);
                const float color[]{trial==2?0.125f:trial?0.75f:0.25f,trial==2?0.25f:0.5f,trial==2?0.5f:trial?0.25f:0.75f,1};
                NVN(nvnSamplerBuilderSetBorderColor)(&builder,color);
                NVN(nvnSamplerBuilderSetWrapMode)(&builder,NVN_WRAP_MODE_CLAMP_TO_BORDER,NVN_WRAP_MODE_CLAMP_TO_BORDER,NVN_WRAP_MODE_CLAMP_TO_BORDER);
                NVN(nvnSamplerPoolRegisterSamplerBuilder)(&samplers,trial%2+1,&builder);
                const NVNtexture *target=&texture;
                NVN(nvnCommandBufferBeginRecording)(&cb);
                NVN(nvnCommandBufferSetRenderTargets)(&cb,1,&target,nullptr,nullptr,nullptr);
                NVN(nvnCommandBufferSetViewport)(&cb,0,0,64,32);NVN(nvnCommandBufferSetScissor)(&cb,0,0,64,32);
                NVN(nvnCommandBufferSetTexturePool)(&cb,&textures);NVN(nvnCommandBufferSetSamplerPool)(&cb,&samplers);
                NVN(nvnCommandBufferBindTexture)(&cb,NVN_SHADER_STAGE_FRAGMENT,0,NVN(nvnDeviceGetTextureHandle)(&nd,1,trial%2+1));
                NVN(nvnCommandBufferBindProgram)(&cb,&border_program,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
                NVN(nvnCommandBufferDrawElementsBaseVertex)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,NVN_INDEX_TYPE_UNSIGNED_SHORT,3,uintptr_t(indices),-1);
                auto command=NVN(nvnCommandBufferEndRecording)(&cb);NVN(nvnQueueSubmitCommands)(&nq,1,&command);NVN(nvnQueueFinish)(&nq);
                NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
                for(size_t i=0;i<readback.size();i+=4){
                    const unsigned x=(i/4)%64;const float weight=x<16?1.f:x<32 || x>=48?.5f:0.f;
                    for(unsigned c=0;c<3;++c)need(std::abs(int(readback[i+c])-int(std::lround(color[c]*weight*255)))<=1,"Filtered custom border footprint or sampler cache mismatch");
                    need(readback[i+3]==255,"Filtered custom border alpha mismatch");
                }
                xbg_game_stats border_stats{};xbg_game_get_stats(&border_stats);
                if(trial)need(border_stats.shader_loads==first_border_shader_loads,"Border color changes must not compile shader variants");
                else first_border_shader_loads=border_stats.shader_loads;
                border_pixels+=64*32;
            }
            stages[1].control=controls[3].data();
            need(NVN(nvnProgramSetShaders)(&border_program,2,stages),"Restore constant sample shader");
            // Distinct sampled views must share pixels without sharing their
            // component mapping, including after a cached view is reused.
            std::array<uint8_t,64> texels{};
            for(unsigned i=0;i<texels.size();i+=4){texels[i]=37;texels[i+1]=83;texels[i+2]=149;texels[i+3]=211;}
            NVNcopyRegion source_region{0,0,0,4,4,1};
            NVN(nvnTextureWriteTexels)(&sampled,nullptr,&source_region,texels.data());
            NVN(nvnTextureFlushTexels)(&sampled,nullptr,&source_region);
            NVNsamplerBuilder edge{};NVN(nvnSamplerBuilderSetDefaults)(&edge);
            NVN(nvnSamplerBuilderSetWrapMode)(&edge,NVN_WRAP_MODE_CLAMP_TO_EDGE,NVN_WRAP_MODE_CLAMP_TO_EDGE,NVN_WRAP_MODE_CLAMP_TO_EDGE);
            NVN(nvnSamplerPoolRegisterSamplerBuilder)(&samplers,1,&edge);
            const uint8_t expected[4][4]{{37,83,149,211},{149,37,83,255},{37,83,149,211},{0,149,255,37}};
            for(unsigned trial=0;trial<4;++trial){
                NVNtextureView view{};NVN(nvnTextureViewSetDefaults)(&view);
                if(trial==1)NVN(nvnTextureViewSetSwizzle)(&view,NVN_TEXTURE_SWIZZLE_B,NVN_TEXTURE_SWIZZLE_R,NVN_TEXTURE_SWIZZLE_G,NVN_TEXTURE_SWIZZLE_ONE);
                if(trial==3)NVN(nvnTextureViewSetSwizzle)(&view,NVN_TEXTURE_SWIZZLE_ZERO,NVN_TEXTURE_SWIZZLE_B,NVN_TEXTURE_SWIZZLE_ONE,NVN_TEXTURE_SWIZZLE_R);
                NVN(nvnTexturePoolRegisterTexture)(&textures,1,&sampled,&view);
                const NVNtexture *target=&texture;
                NVN(nvnCommandBufferBeginRecording)(&cb);
                NVN(nvnCommandBufferBindProgram)(&cb,&border_program,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
                NVN(nvnCommandBufferSetRenderTargets)(&cb,1,&target,nullptr,nullptr,nullptr);
                NVN(nvnCommandBufferSetTexturePool)(&cb,&textures);NVN(nvnCommandBufferSetSamplerPool)(&cb,&samplers);
                NVN(nvnCommandBufferBindTexture)(&cb,NVN_SHADER_STAGE_FRAGMENT,0,NVN(nvnDeviceGetTextureHandle)(&nd,1,1));
                NVN(nvnCommandBufferDrawElementsBaseVertex)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,NVN_INDEX_TYPE_UNSIGNED_SHORT,3,uintptr_t(indices),-1);
                auto command=NVN(nvnCommandBufferEndRecording)(&cb);
                auto *source=static_cast<uint8_t*>(storage)+NVN(nvnTextureGetMemoryOffset)(&sampled);
                const size_t source_bytes=NVN(nvnTextureGetStorageSize)(&sampled);
                const std::vector<uint8_t> saved(source,source+source_bytes);
                const std::array<uint16_t,3> saved_indices{indices[0],indices[1],indices[2]};
                {OrderedProbeGate gate;NVN(nvnQueueSubmitCommands)(&nq,1,&command);gate.check();
                 memset(source,0,source_bytes);indices[0]=indices[1]=indices[2]=0;
                 NVN(nvnDeviceFinalizeCommandHandle)(&nd,command);
                 gate.unblock();NVN(nvnQueueFinish)(&nq);}
                NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
                for(size_t i=0;i<readback.size();i+=4)
                    need(!memcmp(readback.data()+i,expected[trial],4),"Aliased texture view swizzle readback mismatch");
                memcpy(source,saved.data(),source_bytes);memcpy(indices,saved_indices.data(),sizeof(saved_indices));
                swizzle_pixels+=64*32;
            }
            // BOTW's glyph atlas is R8 with ONE/ONE/ONE/R swizzle. The same
            // descriptor is sampled by both 2D and 2D-array shader signatures.
            // Exercise both storage types and alternate signatures without
            // registering the descriptor again, so image-cache hits are tested.
            for(bool array_storage : {false,true}) {
                NVNtexture glyph{};NVNtextureBuilder glyph_builder{};
                NVN(nvnTextureBuilderSetDefaults)(&glyph_builder);
                NVN(nvnTextureBuilderSetDevice)(&glyph_builder,&nd);
                NVN(nvnTextureBuilderSetSize3D)(&glyph_builder,4,4,array_storage?2:1);
                NVN(nvnTextureBuilderSetTarget)(&glyph_builder,array_storage?NVN_TEXTURE_TARGET_2D_ARRAY:NVN_TEXTURE_TARGET_2D);
                NVN(nvnTextureBuilderSetFormat)(&glyph_builder,NVN_FORMAT_R8);
                NVN(nvnTextureBuilderSetSwizzle)(&glyph_builder,NVN_TEXTURE_SWIZZLE_ONE,NVN_TEXTURE_SWIZZLE_ONE,NVN_TEXTURE_SWIZZLE_ONE,NVN_TEXTURE_SWIZZLE_R);
                NVN(nvnTextureBuilderSetStorage)(&glyph_builder,&pool,(3u<<20)+32768);
                need(NVN(nvnTextureInitialize)(&glyph,&glyph_builder),"Glyph atlas initialize");
                std::array<uint8_t,32> glyphs{};std::fill(glyphs.begin(),glyphs.begin()+16,173);std::fill(glyphs.begin()+16,glyphs.end(),59);
                NVNcopyRegion glyph_region{0,0,0,4,4,array_storage?2:1};
                NVN(nvnTextureWriteTexels)(&glyph,nullptr,&glyph_region,glyphs.data());
                NVN(nvnTexturePoolRegisterTexture)(&textures,1,&glyph,nullptr);
                for(unsigned shader_index : {8u,3u,8u,3u}) {
                    stages[1].control=controls[shader_index].data();
                    need(NVN(nvnProgramSetShaders)(&border_program,2,stages),"Glyph shader signature");
                    const NVNtexture *target=&texture;
                    NVN(nvnCommandBufferBeginRecording)(&cb);
                    NVN(nvnCommandBufferSetRenderTargets)(&cb,1,&target,nullptr,nullptr,nullptr);
                    NVN(nvnCommandBufferBindProgram)(&cb,&border_program,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
                    NVN(nvnCommandBufferBindTexture)(&cb,NVN_SHADER_STAGE_FRAGMENT,0,NVN(nvnDeviceGetTextureHandle)(&nd,1,1));
                    NVN(nvnCommandBufferDrawElementsBaseVertex)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,NVN_INDEX_TYPE_UNSIGNED_SHORT,3,uintptr_t(indices),-1);
                    auto command=NVN(nvnCommandBufferEndRecording)(&cb);NVN(nvnQueueSubmitCommands)(&nq,1,&command);NVN(nvnQueueFinish)(&nq);
                    NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
                    const uint8_t expected_glyph[]{255,255,255,uint8_t(array_storage && shader_index==8?59:173)};
                    for(size_t i=0;i<readback.size();i+=4)
                        need(!memcmp(readback.data()+i,expected_glyph,4),"Glyph 2D/array signature, layer or cached view mismatch");
                }
                NVN(nvnTextureFinalize)(&glyph);
            }
            stages[1].control=controls[3].data();
            need(NVN(nvnProgramSetShaders)(&border_program,2,stages),"Restore 2D sample signature");
            // Padded CPU height tiles must retain row order on upload. Test each
            // independent linear flag, automatic/explicit pitch, CPU subrect
            // writes and CPU readback before sampling the bottom-right texel.
            for(int flags : {0x10,0x40,0x50}) for(int mode=0;mode<2;++mode){
                NVNtexture linear{};NVNtextureBuilder linear_builder{};
                NVN(nvnTextureBuilderSetDefaults)(&linear_builder);
                NVN(nvnTextureBuilderSetDevice)(&linear_builder,&nd);
                NVN(nvnTextureBuilderSetSize2D)(&linear_builder,17,9);
                NVN(nvnTextureBuilderSetFormat)(&linear_builder,mode?NVN_FORMAT_RGBA8:NVN_FORMAT_R16);
                NVN(nvnTextureBuilderSetFlags)(&linear_builder,flags);
                const unsigned bpp=mode?4:2, alignment=flags&0x40?128:32;
                const unsigned pitch=mode?256:((17*bpp+alignment-1)&~(alignment-1));
                if(mode)NVN(nvnTextureBuilderSetStride)(&linear_builder,pitch);
                const size_t offset=(3u<<20)+65536;
                NVN(nvnTextureBuilderSetStorage)(&linear_builder,&pool,offset);
                need(NVN(nvnTextureBuilderGetStorageAlignment)(&linear_builder)==alignment &&
                     NVN(nvnTextureBuilderGetStorageSize)(&linear_builder)==pitch*9,"Linear layout pitch/alignment");
                auto *backing=static_cast<uint8_t*>(storage)+offset;memset(backing,0xa5,pitch*9);
                for(unsigned y=0;y<9;++y)memset(backing+y*pitch,0,17*bpp);
                need(NVN(nvnTextureInitialize)(&linear,&linear_builder),"Linear texture initialize");
                const uint8_t rgba[]{37,83,149,211};const uint16_t height=16384;
                NVNcopyRegion pixel{16,8,0,1,1,1},full{0,0,0,17,9,1};
                NVN(nvnTextureWriteTexels)(&linear,nullptr,&pixel,mode?static_cast<const void*>(rgba):&height);
                need(!memcmp(backing+pitch*8+16*bpp,mode?static_cast<const void*>(rgba):&height,bpp),"Linear subrectangle write");
                for(unsigned y=0;y<9;++y)for(unsigned x=17*bpp;x<pitch;++x)
                    need(backing[y*pitch+x]==0xa5,"Linear upload changed padding");
                std::vector<uint8_t> cpu(17*9*bpp);
                NVN(nvnTextureReadTexels)(&linear,nullptr,&full,cpu.data());
                need(!memcmp(cpu.data()+16*bpp+8*17*bpp,mode?static_cast<const void*>(rgba):&height,bpp),"Linear CPU readback");
                NVN(nvnTexturePoolRegisterTexture)(&textures,1,&linear,nullptr);
                NVN(nvnCommandBufferBeginRecording)(&cb);
                NVN(nvnCommandBufferBindTexture)(&cb,NVN_SHADER_STAGE_FRAGMENT,0,NVN(nvnDeviceGetTextureHandle)(&nd,1,1));
                NVN(nvnCommandBufferDrawElementsBaseVertex)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,NVN_INDEX_TYPE_UNSIGNED_SHORT,3,uintptr_t(indices),-1);
                auto command=NVN(nvnCommandBufferEndRecording)(&cb);NVN(nvnQueueSubmitCommands)(&nq,1,&command);NVN(nvnQueueFinish)(&nq);
                NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
                const uint8_t r16[]{64,0,0,255};
                for(size_t i=0;i<readback.size();i+=4){
                    if(memcmp(readback.data()+i,mode?rgba:r16,4))
                        fprintf(stderr,"Linear sample flags=%#x mode=%d pixel=%zu actual=%u/%u/%u/%u expected=%u/%u/%u/%u\n",flags,mode,i/4,readback[i],readback[i+1],readback[i+2],readback[i+3],(mode?rgba:r16)[0],(mode?rgba:r16)[1],(mode?rgba:r16)[2],(mode?rgba:r16)[3]);
                    need(!memcmp(readback.data()+i,mode?rgba:r16,4),"Linear texture GPU sampling");
                }
                linear_pixels+=64*32;
                NVN(nvnTextureFinalize)(&linear);
            }
            // A bound color slot with writes disabled can also be sampled.
            // Repeated draws must preserve its pixels without ending one pass
            // per draw. A mask change and a clear still take effect immediately.
            {
                NVNtexture readonly_color{};NVNtextureBuilder builder{};
                NVN(nvnTextureBuilderSetDefaults)(&builder);NVN(nvnTextureBuilderSetDevice)(&builder,&nd);
                NVN(nvnTextureBuilderSetSize2D)(&builder,64,32);NVN(nvnTextureBuilderSetFormat)(&builder,NVN_FORMAT_RGBA8);
                NVN(nvnTextureBuilderSetStorage)(&builder,&pool,(3u<<20)+131072);
                need(NVN(nvnTextureInitialize)(&readonly_color,&builder),"Read-only color attachment");
                std::vector<uint8_t> green_texels(64*32*4);
                for(unsigned i=0;i<green_texels.size();i+=4){green_texels[i+1]=255;green_texels[i+3]=255;}
                NVN(nvnTextureWriteTexels)(&readonly_color,nullptr,&region,green_texels.data());
                NVN(nvnTexturePoolRegisterTexture)(&textures,1,&readonly_color,nullptr);
                NVNchannelMaskState masks{};NVN(nvnChannelMaskStateSetDefaults)(&masks);
                NVN(nvnChannelMaskStateSetChannelMask)(&masks,1,0,0,0,0);
                const NVNtexture *targets[]{&texture,&readonly_color};
                NVN(nvnCommandBufferBeginRecording)(&cb);
                NVN(nvnCommandBufferSetRenderTargets)(&cb,2,targets,nullptr,nullptr,nullptr);
                NVN(nvnCommandBufferBindChannelMaskState)(&cb,&masks);
                NVN(nvnCommandBufferBindTexture)(&cb,NVN_SHADER_STAGE_FRAGMENT,0,NVN(nvnDeviceGetTextureHandle)(&nd,1,1));
                for(unsigned i=0;i<16;++i){
                    if(const char *defer=getenv("MNG_DEFER_FRAGMENT_BARRIER");i && defer && !strcmp(defer,"1")){
                        const uint32_t dependencies[]{18,17,32,1,255};
                        NVN(nvnCommandBufferBarrier)(&cb,dependencies[(i-1)%5]);
                    }
                    NVN(nvnCommandBufferDrawElementsBaseVertex)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,NVN_INDEX_TYPE_UNSIGNED_SHORT,3,uintptr_t(indices),-1);
                }
                const uint64_t before=mng_nvn_render_pass_begins();
                auto command=NVN(nvnCommandBufferEndRecording)(&cb);NVN(nvnQueueSubmitCommands)(&nq,1,&command);NVN(nvnQueueFinish)(&nq);
                need(mng_nvn_render_pass_begins()==before+1,"Sample-only color attachment split consecutive draws");
                NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
                need(readback==green_texels,"Read-only attachment sample pixels");
                NVN(nvnTextureReadTexels)(&readonly_color,nullptr,&region,readback.data());
                need(readback==green_texels,"Read-only attachment contents changed");
                if(const char *feedback=getenv("MNG_FEEDBACK_SNAPSHOT");feedback && !strcmp(feedback,"1")){
                    // A non-sampled target can stay attached while its mask
                    // changes; these draws require no store/load boundary.
                    NVN(nvnCommandBufferBeginRecording)(&cb);
                    NVN(nvnCommandBufferDrawElementsBaseVertex)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,NVN_INDEX_TYPE_UNSIGNED_SHORT,3,uintptr_t(indices),-1);
                    NVN(nvnChannelMaskStateSetChannelMask)(&masks,0,0,0,0,0);
                    NVN(nvnCommandBufferBindChannelMaskState)(&cb,&masks);
                    NVN(nvnCommandBufferDrawElementsBaseVertex)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,NVN_INDEX_TYPE_UNSIGNED_SHORT,3,uintptr_t(indices),-1);
                    NVN(nvnChannelMaskStateSetChannelMask)(&masks,0,1,1,1,1);
                    NVN(nvnCommandBufferBindChannelMaskState)(&cb,&masks);
                    NVN(nvnCommandBufferDrawElementsBaseVertex)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,NVN_INDEX_TYPE_UNSIGNED_SHORT,3,uintptr_t(indices),-1);
                    const uint64_t start=mng_nvn_render_pass_begins();
                    command=NVN(nvnCommandBufferEndRecording)(&cb);NVN(nvnQueueSubmitCommands)(&nq,1,&command);NVN(nvnQueueFinish)(&nq);
                    need(mng_nvn_render_pass_begins()==start+1,"Unrelated mask changes split one target pass");
                    NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
                    need(readback==green_texels,"Mask-transition pass pixels");
                }
                NVN(nvnCommandBufferBeginRecording)(&cb);
                NVN(nvnCommandBufferClearColor)(&cb,0,blue,15);
                NVN(nvnChannelMaskStateSetChannelMask)(&masks,0,0,0,0,0);
                NVN(nvnCommandBufferBindChannelMaskState)(&cb,&masks);
                NVN(nvnCommandBufferDrawElementsBaseVertex)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,NVN_INDEX_TYPE_UNSIGNED_SHORT,3,uintptr_t(indices),-1);
                NVN(nvnChannelMaskStateSetChannelMask)(&masks,0,1,1,1,1);
                NVN(nvnCommandBufferBindChannelMaskState)(&cb,&masks);
                NVN(nvnCommandBufferDrawElementsBaseVertex)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,NVN_INDEX_TYPE_UNSIGNED_SHORT,3,uintptr_t(indices),-1);
                command=NVN(nvnCommandBufferEndRecording)(&cb);NVN(nvnQueueSubmitCommands)(&nq,1,&command);NVN(nvnQueueFinish)(&nq);
                NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
                need(readback==green_texels,"Mask re-enable reused an attachment-free pass");
                NVN(nvnCommandBufferBeginRecording)(&cb);
                NVN(nvnCommandBufferClearColor)(&cb,1,red,15);
                NVN(nvnCommandBufferDrawElementsBaseVertex)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,NVN_INDEX_TYPE_UNSIGNED_SHORT,3,uintptr_t(indices),-1);
                command=NVN(nvnCommandBufferEndRecording)(&cb);NVN(nvnQueueSubmitCommands)(&nq,1,&command);NVN(nvnQueueFinish)(&nq);
                NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
                for(unsigned i=0;i<readback.size();i+=4)need(readback[i]==255 && readback[i+1]==0 && readback[i+2]==0 && readback[i+3]==255,"Clear of masked attachment lost before sampling");
                NVN(nvnTextureFinalize)(&readonly_color);
                NVN(nvnChannelMaskStateSetDefaults)(&masks);
                NVN(nvnCommandBufferBeginRecording)(&cb);NVN(nvnCommandBufferBindChannelMaskState)(&cb,&masks);
                command=NVN(nvnCommandBufferEndRecording)(&cb);NVN(nvnQueueSubmitCommands)(&nq,1,&command);
            }
            // Depth bound for later draws must not create a feedback loop
            // while all raster depth/stencil tests are disabled.
            {
                NVNtexture depth_sample{};NVNtextureBuilder db{};
                NVN(nvnTextureBuilderSetDefaults)(&db);NVN(nvnTextureBuilderSetDevice)(&db,&nd);
                NVN(nvnTextureBuilderSetSize2D)(&db,64,32);NVN(nvnTextureBuilderSetFormat)(&db,NVN_FORMAT_DEPTH16);
                NVN(nvnTextureBuilderSetStorage)(&db,&pool,(3u<<20)+196608);
                need(NVN(nvnTextureInitialize)(&depth_sample,&db),"Sample-only depth initialize");
                NVN(nvnTexturePoolRegisterTexture)(&textures,1,&depth_sample,nullptr);
                NVN(nvnTexturePoolRegisterTexture)(&textures,2,&sampled,nullptr);
                NVNdepthStencilState ds{};NVN(nvnDepthStencilStateSetDefaults)(&ds);
                NVN(nvnDepthStencilStateSetDepthTestEnable)(&ds,0);
                const NVNtexture *target=&texture;
                auto draw_depth_sample=[&]{NVN(nvnCommandBufferDrawElementsBaseVertex)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,NVN_INDEX_TYPE_UNSIGNED_SHORT,3,uintptr_t(indices),-1);};
                NVN(nvnCommandBufferBeginRecording)(&cb);
                NVN(nvnCommandBufferSetRenderTargets)(&cb,1,&target,nullptr,&depth_sample,nullptr);
                NVN(nvnCommandBufferBindDepthStencilState)(&cb,&ds);
                NVN(nvnCommandBufferClearDepthStencil)(&cb,0.25f,1,0,0);
                auto command=NVN(nvnCommandBufferEndRecording)(&cb);NVN(nvnQueueSubmitCommands)(&nq,1,&command);NVN(nvnQueueFinish)(&nq);
                NVN(nvnCommandBufferBeginRecording)(&cb);
                NVN(nvnCommandBufferBindTexture)(&cb,NVN_SHADER_STAGE_FRAGMENT,0,NVN(nvnDeviceGetTextureHandle)(&nd,1,1));
                for(unsigned i=0;i<16;++i)draw_depth_sample();
                const uint64_t before=mng_nvn_render_pass_begins();
                command=NVN(nvnCommandBufferEndRecording)(&cb);NVN(nvnQueueSubmitCommands)(&nq,1,&command);NVN(nvnQueueFinish)(&nq);
                need(mng_nvn_render_pass_begins()==before+1,"Sample-only depth split consecutive draws");
                NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
                for(unsigned i=0;i<readback.size();i+=4)need(readback[i]==64 && readback[i+3]==255,"Sample-only depth pixels");
                NVN(nvnCommandBufferBeginRecording)(&cb);
                NVN(nvnCommandBufferClearColor)(&cb,0,blue,15);
                NVN(nvnDepthStencilStateSetDepthTestEnable)(&ds,1);
                NVN(nvnDepthStencilStateSetDepthWriteEnable)(&ds,0);
                NVN(nvnDepthStencilStateSetDepthFunc)(&ds,NVN_DEPTH_FUNC_NEVER);
                NVN(nvnCommandBufferBindDepthStencilState)(&cb,&ds);
                NVN(nvnCommandBufferBindTexture)(&cb,NVN_SHADER_STAGE_FRAGMENT,0,NVN(nvnDeviceGetTextureHandle)(&nd,2,1));draw_depth_sample();
                command=NVN(nvnCommandBufferEndRecording)(&cb);NVN(nvnQueueSubmitCommands)(&nq,1,&command);NVN(nvnQueueFinish)(&nq);
                NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
                for(unsigned i=0;i<readback.size();i+=4)need(readback[i]==0 && readback[i+1]==0 && readback[i+2]==255 && readback[i+3]==255,"Depth re-enable reused an attachment-free pass");
                NVN(nvnDepthStencilStateSetDefaults)(&ds);
                NVN(nvnCommandBufferBeginRecording)(&cb);NVN(nvnCommandBufferBindDepthStencilState)(&cb,&ds);
                NVN(nvnCommandBufferSetRenderTargets)(&cb,1,&target,nullptr,nullptr,nullptr);
                command=NVN(nvnCommandBufferEndRecording)(&cb);NVN(nvnQueueSubmitCommands)(&nq,1,&command);
                NVN(nvnTextureFinalize)(&depth_sample);
                fprintf(stderr,"[probe] unused depth: 4096 sampled/re-enabled pixels; sixteen draws in one pass\n");
            }
            if(const char *feedback=getenv("MNG_FEEDBACK_SNAPSHOT");feedback && !strcmp(feedback,"1")) {
                // Two unsynchronized draws read the same run-entry contents.
                // Swizzling R/B makes an accidental per-draw refresh observable.
                NVNtextureView view{};NVN(nvnTextureViewSetDefaults)(&view);
                NVN(nvnTextureViewSetSwizzle)(&view,NVN_TEXTURE_SWIZZLE_B,NVN_TEXTURE_SWIZZLE_G,NVN_TEXTURE_SWIZZLE_R,NVN_TEXTURE_SWIZZLE_A);
                NVN(nvnTexturePoolRegisterTexture)(&textures,1,&texture,&view);
                const NVNtexture *target=&texture;const float green[]{0,1,0,1};
                auto draw_feedback=[&]{NVN(nvnCommandBufferDrawElementsBaseVertex)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,NVN_INDEX_TYPE_UNSIGNED_SHORT,3,uintptr_t(indices),-1);};
                for(unsigned trial=0;trial<7;++trial){
                    NVN(nvnCommandBufferBeginRecording)(&cb);
                    NVN(nvnCommandBufferSetRenderTargets)(&cb,1,&target,nullptr,nullptr,nullptr);
                    NVN(nvnCommandBufferBindTexture)(&cb,NVN_SHADER_STAGE_FRAGMENT,0,NVN(nvnDeviceGetTextureHandle)(&nd,1,1));
                    NVN(nvnCommandBufferClearColor)(&cb,0,red,15);
                    draw_feedback();draw_feedback();
                    if(trial==1){NVN(nvnCommandBufferBarrier)(&cb,NVN_BARRIER_ORDER_FRAGMENTS_BIT|NVN_BARRIER_INVALIDATE_TEXTURE_BIT);draw_feedback();}
                    if(trial>=4){const uint32_t dependencies[]{17,32,255};NVN(nvnCommandBufferBarrier)(&cb,dependencies[trial-4]);draw_feedback();}
                    if(trial==2){NVN(nvnCommandBufferClearColor)(&cb,0,green,15);draw_feedback();}
                    if(trial==3){
                        // A plain draw from a different image must invalidate
                        // the old run even without changing the render target.
                        NVN(nvnTexturePoolRegisterTexture)(&textures,2,&sampled,nullptr);
                        NVN(nvnCommandBufferBindTexture)(&cb,NVN_SHADER_STAGE_FRAGMENT,0,NVN(nvnDeviceGetTextureHandle)(&nd,2,1));draw_feedback();
                        NVN(nvnCommandBufferBindTexture)(&cb,NVN_SHADER_STAGE_FRAGMENT,0,NVN(nvnDeviceGetTextureHandle)(&nd,1,1));draw_feedback();
                    }
                    const uint64_t before=mng_nvn_render_pass_begins();
                    auto command=NVN(nvnCommandBufferEndRecording)(&cb);NVN(nvnQueueSubmitCommands)(&nq,1,&command);NVN(nvnQueueFinish)(&nq);
                    if(!trial){const auto delta=mng_nvn_render_pass_begins()-before;if(delta!=2)fprintf(stderr,"[probe] feedback pass delta=%llu expected=2\n",(unsigned long long)delta);need(delta==2,"Feedback draws did not share one pass after clear");}
                    NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
                    const uint8_t colors[4][4]{{0,0,255,255},{255,0,0,255},{0,255,0,255},{149,83,37,211}};
                    for(unsigned i=0;i<readback.size();i+=4)need(!memcmp(readback.data()+i,colors[trial>=4?1:trial],4),"Feedback run/barrier/clear/plain-write pixels");
                }
                fprintf(stderr,"[probe] feedback runs: 14336 pixels, merged draws, barrier combinations, clear and plain-write invalidation verified\n");
            }
            NVN(nvnProgramFinalize)(&border_program);NVN(nvnTextureFinalize)(&sampled);
        }
        // Array UBO sources are refreshed on memo hits, including across slot
        // reuse and CPU flushes. Four scissors retain distinct per-draw values.
        {
            NVNprogram uniforms{};
            need(NVN(nvnProgramInitialize)(&uniforms,&nd),"Dynamic UBO program");
            NVNshaderData stages[]{{0,controls[0].data()},{0,controls[9].data()}};
            need(NVN(nvnProgramSetShaders)(&uniforms,2,stages),"Dynamic UBO shaders");
            auto *data=static_cast<uint8_t*>(storage)+(3u<<20);
            NVNcolorState color{};NVN(nvnColorStateSetDefaults)(&color);
            for(unsigned trial=0;trial<8;++trial){
                for(unsigned stripe=0;stripe<4;++stripe){
                    const float r=float((trial+stripe)&1),g=float((trial>>1)&1),b=float((stripe>>1)&1);
                    const float values[3][4]{{0,g,0,0},{r,0,0,0},{0,0,b,0}};
                    for(unsigned k=0;k<3;++k)memcpy(data+stripe*768+k*256,values[k],16);
                }
                NVN(nvnMemoryPoolFlushMappedRange)(&pool,3u<<20,3072);
                NVN(nvnCommandBufferBeginRecording)(&cb);
                const NVNtexture* target=&texture;
                NVN(nvnCommandBufferSetRenderTargets)(&cb,1,&target,nullptr,nullptr,nullptr);
                NVN(nvnCommandBufferSetViewport)(&cb,0,0,64,32);
                NVN(nvnCommandBufferBindColorState)(&cb,&color);
                NVN(nvnCommandBufferBindProgram)(&cb,&uniforms,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
                for(unsigned stripe=0;stripe<4;++stripe){
                    NVN(nvnCommandBufferSetScissor)(&cb,stripe*16,0,16,32);
                    for(unsigned k=0;k<3;++k)
                        NVN(nvnCommandBufferBindUniformBuffer)(&cb,NVN_SHADER_STAGE_FRAGMENT,k?6+k:3,uintptr_t(data+stripe*768+k*256),16);
                    NVN(nvnCommandBufferDrawArrays)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
                }
                auto command=NVN(nvnCommandBufferEndRecording)(&cb);
                {
                    OrderedProbeGate gate;
                    NVN(nvnQueueSubmitCommands)(&nq,1,&command);gate.check();
                    memset(data,0,3072);
                    NVN(nvnDeviceFinalizeCommandHandle)(&nd,command);
                    ordered_fence_test(&nd,&nq,gate);gate.unblock();
                    NVN(nvnQueueFinish)(&nq);
                }
                NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
                for(unsigned y=0;y<32;++y)for(unsigned x=0;x<64;++x){
                    const unsigned stripe=x/16;
                    const uint8_t expected[]{uint8_t(((trial+stripe)&1)*255),uint8_t(((trial>>1)&1)*255),uint8_t(((stripe>>1)&1)*255),255};
                    need(!memcmp(readback.data()+(y*64+x)*4,expected,4),"Dynamic UBO source/order/slot reuse mismatch");
                }
            }
            // Rebind identical state four times per submission. Across submits,
            // mutate the contents at the SAME addresses and vary the bound size.
            // The preparation identity may be reused; the actual pixels may not.
            const bool plans=getenv("MNG_PREPARATION_PLAN") && !strcmp(getenv("MNG_PREPARATION_PLAN"),"1");
            for(unsigned trial=0;trial<4;++trial){
                if(plans){char status[2048]{};need(mng_nvn_schedule(-1,-1,-1,-1,-1,-1,-1,-1,trial==1?0:1,-1,status,sizeof status)>=0,"Preparation hot toggle");
                    need(strstr(status,trial==1?"preparation_plan=0":"preparation_plan=1"),"Preparation mode status");}
                const float r=float(trial&1),g=float((trial>>1)&1),b=1-r;
                const float values[3][4]{{0,g,0,0},{r,0,0,0},{0,0,b,0}};
                for(unsigned k=0;k<3;++k)memcpy(data+k*256,values[k],16);
                NVN(nvnMemoryPoolFlushMappedRange)(&pool,3u<<20,768);
                NVN(nvnCommandBufferBeginRecording)(&cb);
                const NVNtexture* target=&texture;
                NVN(nvnCommandBufferSetRenderTargets)(&cb,1,&target,nullptr,nullptr,nullptr);
                NVN(nvnCommandBufferSetViewport)(&cb,0,0,64,32);NVN(nvnCommandBufferSetScissor)(&cb,0,0,64,32);
                for(unsigned repeat=0;repeat<4;++repeat){
                    NVN(nvnCommandBufferBindColorState)(&cb,&color);
                    NVN(nvnCommandBufferBindProgram)(&cb,&uniforms,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
                    for(unsigned k=0;k<3;++k)NVN(nvnCommandBufferBindUniformBuffer)(&cb,NVN_SHADER_STAGE_FRAGMENT,k?6+k:3,uintptr_t(data+k*256),(trial&1)?32:16);
                    NVN(nvnCommandBufferDrawArrays)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
                }
                auto command=NVN(nvnCommandBufferEndRecording)(&cb);
                NVN(nvnQueueSubmitCommands)(&nq,1,&command);NVN(nvnQueueFinish)(&nq);
                NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
                const uint8_t wanted[]{uint8_t(r*255),uint8_t(g*255),uint8_t(b*255),255};
                for(size_t i=0;i<readback.size();i+=4)need(!memcmp(readback.data()+i,wanted,4),"Preparation reused stale same-address UBO data");
            }
            // The next packet deliberately inherits the program and all UBO
            // bindings, but must capture new contents at those same addresses.
            for(unsigned trial=0;trial<4;++trial){
                const float values[3][4]{{0,float(trial&1),0,0},{float((trial>>1)&1),0,0,0},{0,0,1,0}};
                for(unsigned k=0;k<3;++k)memcpy(data+k*256,values[k],16);
                NVN(nvnMemoryPoolFlushMappedRange)(&pool,3u<<20,768);
                NVN(nvnCommandBufferBeginRecording)(&cb);
                NVN(nvnCommandBufferDrawArrays)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
                auto command=NVN(nvnCommandBufferEndRecording)(&cb);
                {OrderedProbeGate gate;NVN(nvnQueueSubmitCommands)(&nq,1,&command);gate.check();
                 memset(data,0,768);NVN(nvnDeviceFinalizeCommandHandle)(&nd,command);
                 gate.unblock();NVN(nvnQueueFinish)(&nq);}
                NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
                const uint8_t wanted[]{uint8_t(((trial>>1)&1)*255),uint8_t((trial&1)*255),255,255};
                for(size_t i=0;i<readback.size();i+=4)need(!memcmp(readback.data()+i,wanted,4),"Inherited UBO snapshot used stale/live data");
            }
            NVN(nvnProgramFinalize)(&uniforms);
            fprintf(stderr,"[probe] 16384 dynamic UBO pixels, 8192 repeated-state pixels and 8192 inherited UBO pixels; delayed overwrite/handle retirement/fence generation verified\n");
        }
        // Packed signed normals use the full 10:10:10:2 vertex fetch path.
        // Convert [-1,1] to UNORM so readback distinguishes channel order,
        // normalization, the most-negative clamp and signed two-bit alpha.
        NVNprogram packed_program{};
        need(NVN(nvnProgramInitialize)(&packed_program,&nd),"Packed vertex program");
        NVNshaderData packed_stages[]{{0,controls[4].data()},{0,controls[5].data()}};
        need(NVN(nvnProgramSetShaders)(&packed_program,2,packed_stages),"Packed vertex shaders");
        NVNvertexAttribState attr{};NVNvertexStreamState stream{};
        NVN(nvnVertexAttribStateSetDefaults)(&attr);NVN(nvnVertexAttribStateSetFormat)(&attr,NVN_FORMAT_RGB10A2SN,0);
        NVN(nvnVertexAttribStateSetStreamIndex)(&attr,0);
        NVN(nvnVertexStreamStateSetDefaults)(&stream);NVN(nvnVertexStreamStateSetStride)(&stream,4);
        auto *packed=reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(storage)+(1u<<20)+4096);
        const uint32_t cases[]{511u|(512u<<10)|(1u<<30),769u|(255u<<10)|(513u<<20)|(2u<<30)};
        const uint8_t expected[2][4]{{255,0,128,255},{64,191,0,0}};
        for(unsigned trial=0;trial<2;++trial){
            packed[0]=packed[1]=packed[2]=cases[trial];
            NVN(nvnMemoryPoolFlushMappedRange)(&pool,(1u<<20)+4096,12);
            const NVNtexture *target=&texture;
            NVN(nvnCommandBufferBeginRecording)(&cb);
            NVN(nvnCommandBufferSetRenderTargets)(&cb,1,&target,nullptr,nullptr,nullptr);
            NVN(nvnCommandBufferSetViewport)(&cb,0,0,64,32);NVN(nvnCommandBufferSetScissor)(&cb,0,0,64,32);
            NVN(nvnCommandBufferBindVertexAttribState)(&cb,1,&attr);
            NVN(nvnCommandBufferBindVertexStreamState)(&cb,1,&stream);
            NVN(nvnCommandBufferBindVertexBuffer)(&cb,0,uintptr_t(packed),12);
            NVN(nvnCommandBufferBindProgram)(&cb,&packed_program,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
            NVN(nvnCommandBufferDrawArrays)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
            auto command=NVN(nvnCommandBufferEndRecording)(&cb);
            {
                OrderedProbeGate gate;NVN(nvnQueueSubmitCommands)(&nq,1,&command);gate.check();
                packed[0]=packed[1]=packed[2]=0;
                NVN(nvnDeviceFinalizeCommandHandle)(&nd,command);
                gate.unblock();NVN(nvnQueueFinish)(&nq);
            }
            NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
            for(size_t i=0;i<readback.size();++i)
                need(readback[i]==expected[trial][i%4],"Packed signed normalized vertex readback mismatch");
            packed[0]=packed[1]=packed[2]=cases[trial];
            NVN(nvnMemoryPoolFlushMappedRange)(&pool,(1u<<20)+4096,12);
        }
        // BOTW's G-buffer repeats attachments in disabled color slots. Clears
        // of those slots must still reach the same image, including partial
        // masks, while draws must not attach overlapping Metal tile storage.
        NVNchannelMaskState duplicate_mask{};
        NVN(nvnChannelMaskStateSetDefaults)(&duplicate_mask);
        NVN(nvnChannelMaskStateSetChannelMask)(&duplicate_mask,1,0,0,0,0);
        NVN(nvnChannelMaskStateSetChannelMask)(&duplicate_mask,2,0,0,0,0);
        for(unsigned trial=0;trial<3;++trial){
            const NVNtexture* duplicate_targets[]{&texture,&texture,&texture};
            NVN(nvnCommandBufferBeginRecording)(&cb);
            NVN(nvnCommandBufferSetRenderTargets)(&cb,3,duplicate_targets,nullptr,nullptr,nullptr);
            NVN(nvnCommandBufferBindChannelMaskState)(&cb,&duplicate_mask);
            NVN(nvnCommandBufferSetScissor)(&cb,0,0,64,32);
            if(trial==0)NVN(nvnCommandBufferClearColor)(&cb,2,red,15);
            else if(trial==1){
                NVN(nvnCommandBufferSetScissor)(&cb,8,4,16,8);
                NVN(nvnCommandBufferClearColor)(&cb,1,blue,5);
            }else NVN(nvnCommandBufferDrawArrays)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
            auto command=NVN(nvnCommandBufferEndRecording)(&cb);
            NVN(nvnQueueSubmitCommands)(&nq,1,&command);NVN(nvnQueueFinish)(&nq);
            NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
            for(unsigned y=0;y<32;++y)for(unsigned x=0;x<64;++x){
                const bool changed=trial==1 && x>=8 && x<24 && y>=4 && y<12;
                const uint8_t cleared[]{uint8_t(changed?0:255),0,uint8_t(changed?255:0),255};
                need(!memcmp(readback.data()+(y*64+x)*4,trial==2?expected[1]:cleared,4),
                     "Duplicate color slot clear/draw readback mismatch");
            }
        }
        NVN(nvnChannelMaskStateSetDefaults)(&duplicate_mask);
        NVN(nvnCommandBufferBeginRecording)(&cb);
        NVN(nvnCommandBufferBindChannelMaskState)(&cb,&duplicate_mask);
        auto reset_mask=NVN(nvnCommandBufferEndRecording)(&cb);
        NVN(nvnQueueSubmitCommands)(&nq,1,&reset_mask);
        NVNcolorState blended{};NVNblendState blend_state{};
        NVN(nvnColorStateSetDefaults)(&blended);NVN(nvnColorStateSetBlendEnable)(&blended,0,1);
        NVN(nvnBlendStateSetDefaults)(&blend_state);NVN(nvnBlendStateSetBlendTarget)(&blend_state,0);
        NVN(nvnBlendStateSetBlendFunc)(&blend_state,NVN_BLEND_FUNC_CONSTANT_COLOR,NVN_BLEND_FUNC_ZERO,NVN_BLEND_FUNC_ONE,NVN_BLEND_FUNC_ZERO);
        auto dynamic_draw=[&](unsigned mask,float factor,float units){
            NVNchannelMaskState channels{};NVN(nvnChannelMaskStateSetDefaults)(&channels);
            NVN(nvnChannelMaskStateSetChannelMask)(&channels,0,mask&1,(mask>>1)&1,(mask>>2)&1,(mask>>3)&1);
            const float color[]{factor,factor,factor,1};const NVNtexture* target=&texture;
            NVN(nvnCommandBufferBeginRecording)(&cb);
            NVN(nvnCommandBufferSetRenderTargets)(&cb,1,&target,nullptr,nullptr,nullptr);
            NVN(nvnCommandBufferSetScissor)(&cb,0,0,64,32);
            NVN(nvnCommandBufferBindChannelMaskState)(&cb,&channels);
            NVN(nvnCommandBufferBindColorState)(&cb,&blended);
            NVN(nvnCommandBufferBindBlendState)(&cb,&blend_state);
            NVN(nvnCommandBufferSetBlendColor)(&cb,color);
            NVN(nvnCommandBufferSetPolygonOffsetClamp)(&cb,factor,units,0);
            NVN(nvnCommandBufferDrawArrays)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
            auto command=NVN(nvnCommandBufferEndRecording)(&cb);
            NVN(nvnQueueSubmitCommands)(&nq,1,&command);NVN(nvnQueueFinish)(&nq);
        };
        uint64_t dynamic_misses=0;
        for(unsigned trial=0;trial<8;++trial){
            const float factor=0.25f+float(trial)/16;
            dynamic_draw(15,factor,float(trial));
            if(trial)need(mng_nvn_pipeline_misses()==dynamic_misses,"Dynamic scalar state created another pipeline");
            else dynamic_misses=mng_nvn_pipeline_misses();
            NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
            for(size_t i=0;i<readback.size();++i){
                const int wanted=int(float(expected[1][i%4])*(i%4==3?1:factor)+0.5f);
                need(abs(int(readback[i])-wanted)<=1,"Dynamic blend constant pixel mismatch");
            }
        }
        // Also run with MNG_PIPELINE_CACHE_ENTRIES=4: each cold mask evicts
        // older state, but the fast-hit hot pipeline must remain recent.
        for(unsigned mask=1;mask<15;++mask){
            dynamic_draw(mask,0.5f,0);
            const uint64_t before=mng_nvn_pipeline_misses();
            dynamic_draw(15,0.5f,0);
            need(mng_nvn_pipeline_misses()==before,"Fast-cache hits failed to keep the hot pipeline resident");
        }
        NVN(nvnColorStateSetDefaults)(&blended);NVN(nvnChannelMaskStateSetDefaults)(&duplicate_mask);
        NVN(nvnCommandBufferBeginRecording)(&cb);
        NVN(nvnCommandBufferBindColorState)(&cb,&blended);
        NVN(nvnCommandBufferBindChannelMaskState)(&cb,&duplicate_mask);
        NVN(nvnCommandBufferSetPolygonOffsetClamp)(&cb,0,0,0);
        reset_mask=NVN(nvnCommandBufferEndRecording)(&cb);NVN(nvnQueueSubmitCommands)(&nq,1,&reset_mask);
        NVN(nvnVertexStreamStateSetDivisor)(&stream,1);
        packed[0]=512u|(512u<<10)|(511u<<20)|(1u<<30);
        packed[1]=512u|(511u<<10)|(512u<<20)|(1u<<30);
        packed[2]=511u|(511u<<10)|(512u<<20)|(1u<<30);
        NVN(nvnMemoryPoolFlushMappedRange)(&pool,(1u<<20)+4096,12);
        for(unsigned trial=0;trial<3;++trial){
            const NVNtexture *target=&texture;
            NVN(nvnCommandBufferBeginRecording)(&cb);
            NVN(nvnCommandBufferSetRenderTargets)(&cb,1,&target,nullptr,nullptr,nullptr);
            NVN(nvnCommandBufferSetViewport)(&cb,0,0,64,32);NVN(nvnCommandBufferSetScissor)(&cb,0,0,64,32);
            NVN(nvnCommandBufferClearColor)(&cb,0,red,15);
            NVN(nvnCommandBufferBindVertexAttribState)(&cb,1,&attr);
            NVN(nvnCommandBufferBindVertexStreamState)(&cb,1,&stream);
            NVN(nvnCommandBufferBindVertexBuffer)(&cb,0,uintptr_t(packed),12);
            NVN(nvnCommandBufferBindProgram)(&cb,&packed_program,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
            NVN(nvnCommandBufferDrawArraysInstanced)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3,1,trial==2?0:trial+1);
            auto command=NVN(nvnCommandBufferEndRecording)(&cb);NVN(nvnQueueSubmitCommands)(&nq,1,&command);NVN(nvnQueueFinish)(&nq);
            NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
            for(size_t i=0;i<readback.size();i+=4)
                need(readback[i]==(trial==0?0:255) && readback[i+1]==(trial==2?0:255) && readback[i+2]==0 && readback[i+3]==255,
                     "Instanced vertex fetch base/count/zero mismatch");
        }
        NVN(nvnProgramFinalize)(&packed_program);
        unsigned depth_pixels=0;
        if(enabled.depthBounds){
            NVNtexture depth{};
            NVN(nvnTextureBuilderSetDefaults)(&tb);NVN(nvnTextureBuilderSetDevice)(&tb,&nd);
            NVN(nvnTextureBuilderSetSize2D)(&tb,64,32);NVN(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_DEPTH32F);
            NVN(nvnTextureBuilderSetStorage)(&tb,&pool,3u<<20);
            need(NVN(nvnTextureInitialize)(&depth,&tb),"Depth-bounds attachment initialize");
            NVNprogram bounded{};need(NVN(nvnProgramInitialize)(&bounded,&nd),"Depth-bounds program");
            need(NVN(nvnProgramSetShaders)(&bounded,2,shaders),"Depth-bounds shaders");
            for(unsigned trial=0;trial<3;++trial){
                const NVNtexture *target=&texture;
                NVN(nvnCommandBufferBeginRecording)(&cb);
                NVN(nvnCommandBufferSetRenderTargets)(&cb,1,&target,nullptr,&depth,nullptr);
                NVN(nvnCommandBufferSetViewport)(&cb,0,0,64,32);NVN(nvnCommandBufferSetScissor)(&cb,0,0,64,32);
                NVN(nvnCommandBufferClearColor)(&cb,0,red,15);
                NVN(nvnCommandBufferClearDepthStencil)(&cb,0.5f,1,0,0);
                NVN(nvnCommandBufferSetDepthBounds)(&cb,trial!=2,trial?0.75f:0.25f,trial?1.0f:0.75f);
                NVN(nvnCommandBufferBindProgram)(&cb,&bounded,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
                NVN(nvnCommandBufferDrawArrays)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
                NVN(nvnCommandBufferSetDepthBounds)(&cb,0,0,1);
                auto command=NVN(nvnCommandBufferEndRecording)(&cb);NVN(nvnQueueSubmitCommands)(&nq,1,&command);NVN(nvnQueueFinish)(&nq);
                NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
                for(size_t i=0;i<readback.size();i+=4)
                    need(readback[i]==(trial==1?255:0) && readback[i+1]==(trial==1?0:255) && readback[i+2]==0 && readback[i+3]==255,
                         "Depth-bounds acceptance/rejection/disable readback mismatch");
                depth_pixels+=64*32;
            }
            NVN(nvnProgramFinalize)(&bounded);NVN(nvnTextureFinalize)(&depth);
        }
        NVNtexture depth_array{};
        NVN(nvnTextureBuilderSetDefaults)(&tb);NVN(nvnTextureBuilderSetDevice)(&tb,&nd);
        NVN(nvnTextureBuilderSetTarget)(&tb,NVN_TEXTURE_TARGET_2D_ARRAY);
        NVN(nvnTextureBuilderSetSize3D)(&tb,8,8,3);NVN(nvnTextureBuilderSetFormat)(&tb,NVN_FORMAT_DEPTH16);
        NVN(nvnTextureBuilderSetStorage)(&tb,&pool,3u<<20);
        need(NVN(nvnTextureInitialize)(&depth_array,&tb),"Depth array initialize");
        NVNtextureView depth_subset{};NVN(nvnTextureViewSetDefaults)(&depth_subset);
        NVN(nvnTextureViewSetLayers)(&depth_subset,1,2);
        NVN(nvnCommandBufferBeginRecording)(&cb);
        NVN(nvnCommandBufferSetRenderTargets)(&cb,0,nullptr,nullptr,&depth_array,nullptr);
        NVN(nvnCommandBufferSetScissor)(&cb,0,0,8,8);
        NVN(nvnCommandBufferClearDepthStencil)(&cb,0.25f,1,0,0);
        NVN(nvnCommandBufferSetRenderTargets)(&cb,0,nullptr,nullptr,&depth_array,&depth_subset);
        NVN(nvnCommandBufferSetScissor)(&cb,2,2,4,4);
        NVN(nvnCommandBufferClearDepthStencil)(&cb,0.75f,1,0,0);
        auto layered_clear=NVN(nvnCommandBufferEndRecording)(&cb);
        NVN(nvnQueueSubmitCommands)(&nq,1,&layered_clear);NVN(nvnQueueFinish)(&nq);
        NVNcopyRegion depth_region{0,0,0,8,8,3};std::array<uint16_t,8*8*3> depth_readback{};
        NVN(nvnTextureReadTexels)(&depth_array,nullptr,&depth_region,depth_readback.data());
        for(unsigned layer=0;layer<3;++layer)for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){
            bool subset=layer>=1 && x>=2 && x<6 && y>=2 && y<6;
            need(depth_readback[layer*64+y*8+x]==(subset?49151:16384),"Array depth clear layer/subset/scissor mismatch");
        }
        NVN(nvnTextureFinalize)(&depth_array);
        NVNprogram raster{};need(NVN(nvnProgramInitialize)(&raster,&nd),"Raster discard program");
        need(NVN(nvnProgramSetShaders)(&raster,2,shaders),"Raster discard shaders");
        for(unsigned trial=0;trial<3;++trial){
            const NVNtexture *target=&texture;
            NVN(nvnCommandBufferBeginRecording)(&cb);
            NVN(nvnCommandBufferSetRenderTargets)(&cb,1,&target,nullptr,nullptr,nullptr);
            NVN(nvnCommandBufferSetViewport)(&cb,0,0,64,32);NVN(nvnCommandBufferSetScissor)(&cb,0,0,64,32);
            NVN(nvnCommandBufferClearColor)(&cb,0,red,15);
            NVN(nvnCommandBufferSetRasterizerDiscard)(&cb,trial==1);
            NVN(nvnCommandBufferBindProgram)(&cb,&raster,NVN_SHADER_STAGE_VERTEX_BIT|NVN_SHADER_STAGE_FRAGMENT_BIT);
            NVN(nvnCommandBufferDrawArrays)(&cb,NVN_DRAW_PRIMITIVE_TRIANGLES,0,3);
            auto command=NVN(nvnCommandBufferEndRecording)(&cb);NVN(nvnQueueSubmitCommands)(&nq,1,&command);NVN(nvnQueueFinish)(&nq);
            NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
            for(size_t i=0;i<readback.size();i+=4)
                need(readback[i]==(trial==1?255:0) && readback[i+1]==(trial==1?0:255) && readback[i+2]==0 && readback[i+3]==255,
                     "Raster discard enable/disable readback mismatch");
        }
        NVN(nvnProgramFinalize)(&raster);
        // Reuse the same CPU control handle while previous GPU clears remain
        // queued. Only the final read waits; old command bytes may be freed.
        for(unsigned i=0;i<16;++i){
            float stripe[4]={float(i&1),float(!(i&1)),0,1};
            auto command=record_clear(int(i*4),0,4,32,stripe);
            need(command==uintptr_t(control_memory),"Recycled command handle changed identity");
            NVN(nvnQueueSubmitCommands)(&nq,1,&command);
        }
        NVN(nvnDeviceFinalizeCommandHandle)(&nd,uintptr_t(control_memory));
        NVN(nvnQueueFinish)(&nq);
        NVN(nvnTextureReadTexels)(&texture,nullptr,&region,readback.data());
        for(unsigned y=0;y<32;++y)for(unsigned x=0;x<64;++x){
            const auto i=(y*64+x)*4;const bool red=((x/4)&1)!=0;
            need(readback[i]==(red?255:0) && readback[i+1]==(red?0:255) && readback[i+2]==0 && readback[i+3]==255,
                "CPU command retirement lost an in-flight GPU clear");
        }
        fprintf(stderr,"[probe] 16 in-flight control-handle replacements/finalization: 2048 stripe pixels verified\n");
        NVN(nvnCommandBufferFinalize)(&copy);
        NVN(nvnCommandBufferFinalize)(&cb);
        NVN(nvnSyncFinalize)(&sync);
        sync_wait_progress_test(&nd,&nq,&texture);
        range_invalidate_test(&nd,&nq,&pool);
        present_handoff_test(gpu,device,queue,family,&nd,&nq,&pool,&texture);
        xbg_game_stats final_stats{};xbg_game_get_stats(&final_stats);
        NVN(nvnTextureFinalize)(&texture);
        {
            char status[2048]{};need(mng_nvn_publication_control(-1,status,sizeof status)>0,"Publication snapshot");
            if(strstr(status,"publication_queue=1\n")){
                need(!strstr(status,"pending=0\n"),"Finalized texture must be published before the next engine boundary");
                need(mng_nvn_publication_control(0,status,sizeof status)>0,"Disable publication queue");
                need(strstr(status,"publication_queue=0\n") && strstr(status,"pending=0\n") && strstr(status,"in_flight=0\n"),"Disable drains publications");
                need(mng_nvn_publication_control(1,status,sizeof status)>0,"Re-enable publication queue");
            }
        }
        NVN(nvnQueueFinalize)(&nq);
        NVN(nvnMemoryPoolFinalize)(&pool);
        // Pool storage reuse must not resurrect the previous GPU allocation.
        memset(storage, 0x35, bytes);
        need(NVN(nvnMemoryPoolInitialize)(&pool, &pb), "Recycled pool initialization");
        NVN(nvnTextureBuilderSetDefaults)(&tb);
        NVN(nvnTextureBuilderSetDevice)(&tb, &nd);
        NVN(nvnTextureBuilderSetSize2D)(&tb, 64, 32);
        NVN(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
        NVN(nvnTextureBuilderSetStorage)(&tb, &pool, 0);
        need(NVN(nvnTextureInitialize)(&texture, &tb), "Recycled storage texture");
        NVN(nvnTextureReadTexels)(&texture, nullptr, &region, readback.data());
        for (uint8_t value : readback)
            need(value == 0x35, "Recycled pool inherited stale GPU contents");
        NVN(nvnTextureFinalize)(&texture);
        NVN(nvnMemoryPoolFinalize)(&pool);
        virtual_pool_test(&nd);
        NVN(nvnDeviceFinalize)(&nd);
        {
            char cache_report[256]{};
            need(mng_pipeline_cache_probe(cache_report,sizeof cache_report)==0,"Early pipeline cache exercised");
            fprintf(stderr,"[probe] %s\n",cache_report);
        }
        {char preparation_report[512]{};need(mng_preparation_probe(preparation_report,sizeof preparation_report)==0,"Preparation reuse paths exercised");fprintf(stderr,"[probe] %s\n",preparation_report);}
        {char warm_report[256]{};int status=mng_warm_pipeline_probe(warm_report,sizeof warm_report);fprintf(stderr,"[probe] %s status=%d\n",warm_report,status);need(status==0,"Concurrent PSO jobs");}
        {char ordered_report[2048]{};mng_nvn_ordered_status(ordered_report,sizeof ordered_report);fprintf(stderr,"[probe] %s",ordered_report);}
        {char status[2048]{};need(mng_nvn_publication_control(-1,status,sizeof status)>0,"Final publication counters");
         fprintf(stderr,"[publication-probe] %s",status);}
        need(mng_nvn_shutdown() == 0, "Vulkan shutdown");
        initialized = false;
        free(storage);
        storage = nullptr;
        vkDestroyDevice(device, nullptr);
        device = {};
        vkDestroyInstance(instance, nullptr);
        instance = {};
        std::filesystem::remove_all(synthetic_directory);
        synthetic_directory.clear();
        snprintf(report, report_bytes,
                 "PASS: native NVN -> Vulkan on %s; %u submissions, %u BC block words, %u custom-border pixels, %u aliased-swizzle pixels, 4096 packed-SNORM pixels, %u depth-bounds pixels, %u offline test shaders, %u GPU "
                 "draws, %u dispatches, 576 storage words verified, 2048 RGBA pixels verified, 128 "
                 "scissored pixels, 6144 duplicate-attachment clear/draw pixels, 16384 dynamic-blend pixels without pipeline rebuilds, hot-pipeline LRU, oversized buffer cache bounds, 1048580 texture/sampler churn cycles with constant host objects, 1048580 transient copied builders, 4096 concurrent builder/command lifecycles, 6144 instanced-fetch pixels, 192 layered D16 pixels, 6144 raster-discard pixels, signed base vertex, tiled-cache ordering, copied texture lifetimes, short shader-control guard, 66000 recycled texture recordings, 256 GPU-content aliases, %u pitched R16/RGBA8 sample pixels, control-handle reuse and immutable CopyCommands, "
                 "8 volume attachment slices, null command-memory capacity, physical/virtual aliases, atomic mapping validation, GPU fence, readback and compact object/view canaries; BOTW "
                 "shaders/gameplay/WSI not tested; unlocked present completion and 2048 retained copy pixels verified; 16384 glyph 2D/array signature pixels verified\n",
                 properties.deviceName,unsigned(final_stats.submissions),block_words,border_pixels,swizzle_pixels,depth_pixels,unsigned(final_stats.shader_loads),unsigned(final_stats.draws),unsigned(final_stats.dispatches),linear_pixels);
        return 0;
    } catch (const std::exception &e) {
        if (report && report_bytes)
            snprintf(report, report_bytes, "FAIL: %s", e.what());
        if (initialized)
            mng_nvn_shutdown();
        if (device)
            vkDestroyDevice(device, nullptr);
        if (instance)
            vkDestroyInstance(instance, nullptr);
        free(storage);
        if (!synthetic_directory.empty())
            std::filesystem::remove_all(synthetic_directory);
        return 1;
    }
}
