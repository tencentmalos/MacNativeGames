// SPDX-License-Identifier: MIT
#include "nvn_vulkan.h"
#include <vulkan/vulkan_beta.h>
#include "profiler.h"
#include "vulkan_driver.h"
#include "render_record_vulkan.h"
#include "host_platform.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <mutex>
#include <map>
#include <string>
#include <array>
#include <dlfcn.h>
#include <unwind.h>

extern "C" {
[[noreturn]] void runtime_fail(const char*);

void mng_present_initialize(VkInstance,VkPhysicalDevice,VkDevice,VkQueue,uint32_t);
int mng_present_image(void*,void*,VkImage,VkFormat,uint32_t,uint32_t,const int32_t[4],int32_t);
void mng_present_wait();
void* runtime_hle_wrap_named(void*,const char*);
int mng_guest_memory_writable(uint64_t,size_t);
uint64_t runtime_invoke_guest(void*,const uint64_t*,uint32_t);
}
extern "C" char mng_missing_nvn_thunks[];
namespace {
std::mutex missing_mutex;
std::map<std::string,unsigned> missing;
std::array<std::string,1024> missing_names;
void* missing_proc(void* fn,const char* name) {
    if(fn)return runtime_hle_wrap_named(fn,name);
    std::lock_guard lock(missing_mutex);
    auto it=missing.find(name);
    if(it==missing.end()) {
        const auto index=missing.size();if(index==1024)runtime_fail("Missing NVN diagnostic table full");
        missing_names[index]=name;it=missing.emplace(name,unsigned(index)).first;
    }
    return runtime_hle_wrap_named(mng_missing_nvn_thunks+16*it->second,missing_names[it->second].c_str());
}
std::mutex mutex;
VkInstance instance;
VkDevice device;
_Unwind_Reason_Code fatal_frame(_Unwind_Context* context,void* data) {
    auto& count=*static_cast<unsigned*>(data);
    if(count>=24)return _URC_END_OF_STACK;
    const auto pc=uintptr_t(_Unwind_GetIP(context));Dl_info info{};
    if(dladdr(reinterpret_cast<void*>(pc),&info) && info.dli_fbase)
        fprintf(stderr,"[nvn-host-stack] #%u pc=%#llx module=%s offset=%#llx\n",count,
            (unsigned long long)pc,info.dli_fname,(unsigned long long)(pc-uintptr_t(info.dli_fbase)));
    else fprintf(stderr,"[nvn-host-stack] #%u pc=%#llx module=unknown\n",count,(unsigned long long)pc);
    ++count;return _URC_NO_REASON;
}
void check(VkResult r) {if(r!=VK_SUCCESS){char text[96];snprintf(text,sizeof text,"Native Vulkan error %d",r);runtime_fail(text);}}
void initialize(void* memory,unsigned long bytes) {
    if(!memory||!bytes)runtime_fail("Invalid graphics work memory");
    std::lock_guard lock(mutex);if(device)return;
    if(const char* error=mng_vulkan_initialize(nullptr,nullptr,nullptr))runtime_fail(error);
    const char* shaders=getenv("MNG_SHADER_DIR");if(!shaders)runtime_fail("MNG_SHADER_DIR is required for native game shaders");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="MacNativeGames BOTW";app.apiVersion=VK_API_VERSION_1_1;
    const bool window=mng_host_window()!=nullptr;
    std::vector<const char*> extensions;
    if(window){extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);extensions.push_back(mng_host_surface_extension());}
#ifndef NDEBUG
    char capture[256]{};mng_host_option("debug.mng.capture_enable",capture,sizeof capture);
    if(!strcmp(capture,"1")){
        uint32_t count=0;check(vkEnumerateInstanceExtensionProperties(nullptr,&count,nullptr));
        std::vector<VkExtensionProperties> available(count);check(vkEnumerateInstanceExtensionProperties(nullptr,&count,available.data()));
        for(const auto& e:available)if(!strcmp(e.extensionName,VK_EXT_DEBUG_UTILS_EXTENSION_NAME))
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
#endif
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ci.pApplicationInfo=&app;
    ci.enabledExtensionCount=extensions.size();ci.ppEnabledExtensionNames=extensions.data();check(vkCreateInstance(&ci,nullptr,&instance));volkLoadInstance(instance);
    uint32_t n=0;check(vkEnumeratePhysicalDevices(instance,&n,nullptr));if(!n)runtime_fail("No Vulkan GPU");
    std::vector<VkPhysicalDevice> gpus(n);check(vkEnumeratePhysicalDevices(instance,&n,gpus.data()));auto gpu=gpus[0];
    vkGetPhysicalDeviceQueueFamilyProperties(gpu,&n,nullptr);std::vector<VkQueueFamilyProperties> families(n);vkGetPhysicalDeviceQueueFamilyProperties(gpu,&n,families.data());
    uint32_t family=0;while(family<n&&(families[family].queueFlags&3)!=3)++family;if(family==n)runtime_fail("No graphics and compute Vulkan queue");
    uint32_t extension_count=0;check(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&extension_count,nullptr));
    std::vector<VkExtensionProperties> device_extensions(extension_count);check(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&extension_count,device_extensions.data()));
    auto has_extension=[&](const char* name){for(const auto& e:device_extensions)if(!strcmp(e.extensionName,name))return true;return false;};
    const bool custom_border=has_extension(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    const bool calibrated=has_extension(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    const bool mirror_clamp=has_extension(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceShaderDrawParametersFeatures draw{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
    const bool portable=has_extension(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
    VkPhysicalDevicePortabilitySubsetFeaturesKHR portability{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR};
    if(custom_border){draw.pNext=&border;if(portable)border.pNext=&portability;}
    else if(portable)draw.pNext=&portability;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};features.pNext=&draw;vkGetPhysicalDeviceFeatures2(gpu,&features);
    // D2R's Android host enables BC sampling. Its standalone renderer defaults
    // to CPU decode; BOTW's 83-layer array expands eightfold on that fallback.
    // The engine additionally checks each format's sampling/transfer features.
    setenv("DM_GFX_NATIVE_BC",features.features.textureCompressionBC?"1":"0",1);
    fprintf(stderr,"[native-boot] native BC texture capability=%u\n",features.features.textureCompressionBC);
    float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;di.pEnabledFeatures=&features.features;di.pNext=&draw;
    std::vector<const char*> enabled_extensions;
    if(calibrated)enabled_extensions.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
    if(window)enabled_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if(custom_border)enabled_extensions.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if(mirror_clamp)enabled_extensions.push_back(VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME);
    if(has_extension("VK_KHR_portability_subset"))enabled_extensions.push_back("VK_KHR_portability_subset");
    di.enabledExtensionCount=enabled_extensions.size();di.ppEnabledExtensionNames=enabled_extensions.data();
    check(vkCreateDevice(gpu,&di,nullptr,&device));volkLoadDevice(device);mng_vulkan_identify(gpu);VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    xbg_game_config config{};config.size=sizeof config;config.physical=gpu;config.device=device;config.queue=queue;config.queue_family=family;
    config.enabled_core=features.features;config.enabled_shader_draw_parameters=draw.shaderDrawParameters;config.fence_timeout_ns=5000000000ull;config.shader_directory=shaders;
    config.invoke_guest=[](void*,void* fn,const uint64_t* args,uint32_t count){return runtime_invoke_guest(fn,args,count);};
    config.log=[](void*,const char* message){fprintf(stderr,"[nvn-vulkan] %s\n",message);};
    config.fatal=[](void*,const char* message){unsigned frames=0;_Unwind_Backtrace(fatal_frame,&frames);runtime_fail(message);};
    config.present=mng_present_image;
    mng::profile::configureGpu(instance,gpu,device,queue,family,calibrated);
    mng_present_initialize(instance,gpu,device,queue,family);
    char error[512]{};if(mng_nvn_initialize(&config,error,sizeof error))runtime_fail(error);
    mng_nvn_set_present_wait(mng_present_wait);
    mng_nvn_set_present_source_commands();
    mng_nvn_set_custom_border_color(custom_border && border.customBorderColors);
    if(xbg_game_set_sampler_mirror_clamp_to_edge(mirror_clamp))runtime_fail("Sampler mirror-clamp capability setup failed");
#ifndef NDEBUG
    if(!strcmp(capture,"1")) {
        const char* directory=getenv("TOTK_PROFILE_DIR");
        int result=mng_render_record_initialize(instance,gpu,device,queue,directory);
        fprintf(stderr,"[render-record] initialize=%d\n",result);
    }
#endif
    fprintf(stderr,"[native-boot] custom border=%u mirror clamp=%u\n",unsigned(custom_border && border.customBorderColors),unsigned(mirror_clamp));
    mng_nvn_set_host_wrapper(missing_proc);
    mng_nvn_set_memory_validator(mng_guest_memory_writable);
    fprintf(stderr,"[native-boot] native Vulkan graphics initialized\n");
}
void* bootstrap(const char* name) {
    auto fn=mng_nvn_get_proc_address(name);
    if(!fn)fprintf(stderr,"[nvn-missing] %s\n",name);
    return missing_proc(fn,name);
}
struct Allocator {void* alloc;void* free;void* realloc;void* user;};
Allocator game_allocator,dev_allocator;
void allocator(void* a,void* f,void* r,void* u) {game_allocator={a,f,r,u};}
void devtools(void* a,void* f,void* r,void* u) {dev_allocator={a,f,r,u};}
}
extern "C" void* runtime_gfx_override(const char* name) {
    if(!strcmp(name,"nvnBootstrapLoader"))return reinterpret_cast<void*>(bootstrap);
    if(!strcmp(name,"_ZN2nv18InitializeGraphicsEPvm"))return reinterpret_cast<void*>(initialize);
    if(!strcmp(name,"_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_"))return reinterpret_cast<void*>(allocator);
    if(!strcmp(name,"_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_"))return reinterpret_cast<void*>(devtools);
    return nullptr;
}

extern "C" [[noreturn]] void mng_missing_nvn_called(unsigned index) {
    if(index>=missing_names.size())runtime_fail("Missing NVN diagnostic index invalid");
    runtime_fail((std::string("Unimplemented NVN call: ")+missing_names[index]).c_str());
}
