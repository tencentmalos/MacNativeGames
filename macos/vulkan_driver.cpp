// SPDX-License-Identifier: MIT
#include "vulkan_driver.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <fstream>
#include <iterator>
#include "../porting/runtime/port_runtime.h"
namespace {std::mutex mutex;void* library;std::string driver,error;}
const char* mng_vulkan_initialize(const char* kind,const char*,const char* directory) {
    std::lock_guard lock(mutex);
    if(library)return nullptr;
    if(kind && strcmp(kind,"moltenvk"))return "macOS requires the moltenvk driver";
    const char* path=directory&&*directory?directory:getenv("MNG_VULKAN_LIBRARY");
    if(!path || path[0]!='/')return "Set MNG_VULKAN_LIBRARY to an absolute MoltenVK dylib path";
    void* handle=dlopen(path,RTLD_NOW|RTLD_LOCAL);
    if(!handle){error=dlerror();return error.c_str();}
    auto entry=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(handle,"vkGetInstanceProcAddr"));
    if(!entry){dlclose(handle);return "MoltenVK has no vkGetInstanceProcAddr";}
    Dl_info info{};
    if(!dladdr(reinterpret_cast<void*>(entry),&info)||!info.dli_fname){dlclose(handle);return "Cannot identify mapped MoltenVK";}
    std::ifstream file(info.dli_fname,std::ios::binary);
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file),{}};
    if(bytes.empty()){dlclose(handle);return "Cannot hash mapped MoltenVK";}
    driver=info.dli_fname;
    fprintf(stderr,"[mng-vulkan] mapped=%s sha256=%s\n",driver.c_str(),mng::port::sha256(bytes).c_str());
    volkInitializeCustom(entry);library=handle;
    return nullptr;
}
void mng_vulkan_identify(VkPhysicalDevice gpu) {
    VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(gpu,&properties);
    fprintf(stderr,"[mng-vulkan] gpu=%s api=%u driver=%u library=%s\n",properties.deviceName,
        properties.apiVersion,properties.driverVersion,driver.c_str());
}
