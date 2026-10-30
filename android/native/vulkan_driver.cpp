// SPDX-License-Identifier: MIT
#include "vulkan_driver.h"
#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace {
std::mutex mutex;
std::string selection, failure;
void* library;
bool attempted;
}
const char* mng_vulkan_initialize(const char* kind, const char* hooks, const char* directory) {
    std::lock_guard lock(mutex);
    // A null request is used by renderer/probe initialization after explicit
    // selection, or selects the system driver in standalone diagnostic tools.
    if(!kind && attempted) return failure.empty()?nullptr:failure.c_str();
    const std::string requested=kind?kind:"system";
    if(attempted) {
        if(selection!=requested)return "Changing Vulkan driver requires a new game process";
        return failure.empty()?nullptr:failure.c_str();
    }
    attempted=true;selection=requested;
    if(requested=="turnip") {
        if(!hooks || !directory || !*directory)failure="Bundled Turnip is unavailable";
        else {
            std::string hook_dir=std::string(hooks)+"/",driver_dir=std::string(directory)+"/";
            // Same A8xx mitigation settings as the pinned shadPS4 build. These
            // are scoped to this process; no Android system properties change.
            std::string flags=getenv("TU_DEBUG")?getenv("TU_DEBUG"):"";
            for(const char* flag:{"gmem_warmup","roalign64k"}) {
                if(flags.find(flag)==std::string::npos) {if(!flags.empty())flags+=",";flags+=flag;}
            }
            setenv("TU_DEBUG",flags.c_str(),1);
            library=adrenotools_open_libvulkan(RTLD_NOW,ADRENOTOOLS_DRIVER_CUSTOM,nullptr,
                hook_dir.c_str(),driver_dir.c_str(),"vulkan.ad07xx.so",nullptr,nullptr);
            if(!library)failure="Cannot load bundled Turnip; select the system driver in settings";
        }
    } else if(requested=="system") {
        library=dlopen("libvulkan.so",RTLD_NOW|RTLD_LOCAL);
        if(!library)failure="Cannot load the Android Vulkan driver";
    } else failure="Unknown Vulkan driver selection";
    if(failure.empty()) {
        auto entry=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library,"vkGetInstanceProcAddr"));
        if(!entry)failure="Vulkan driver has no vkGetInstanceProcAddr";
        else {
            volkInitializeCustom(entry);
            if(!vkCreateInstance || !vkEnumerateInstanceExtensionProperties)failure="Vulkan global dispatch is incomplete";
        }
    }
    // adrenotools may return a loader even when its ICD hook falls back. Verify
    // the physical driver's identity before entering guest code, not just dlopen.
    if(failure.empty() && requested=="turnip") {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="BOTW driver verification";app.apiVersion=VK_API_VERSION_1_1;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};info.pApplicationInfo=&app;
        VkInstance probe{};
        if(vkCreateInstance(&info,nullptr,&probe)!=VK_SUCCESS)failure="Turnip could not create a Vulkan instance";
        else {
            volkLoadInstance(probe);
            uint32_t count=1;VkPhysicalDevice gpu{};
            auto result=vkEnumeratePhysicalDevices(probe,&count,&gpu);
            if((result!=VK_SUCCESS && result!=VK_INCOMPLETE)||!count)failure="Turnip exposes no usable GPU";
            else {
                VkPhysicalDeviceDriverProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
                VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&identity;
                vkGetPhysicalDeviceProperties2(gpu,&properties);
                fprintf(stderr,"[vulkan-driver] preflight driver_id=%d name=%s info=%s\n",identity.driverID,identity.driverName,identity.driverInfo);
                if(identity.driverID!=VK_DRIVER_ID_MESA_TURNIP)
                    failure="Turnip did not load; system-driver fallback was rejected. Select System GPU driver or repair the APK";
            }
            vkDestroyInstance(probe,nullptr);
        }
    }
    fprintf(stderr,"[vulkan-driver] requested=%s loaded=%d error=%s\n",selection.c_str(),failure.empty(),failure.c_str());
    return failure.empty()?nullptr:failure.c_str();
}
void mng_vulkan_identify(VkPhysicalDevice gpu) {
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    // Properties promoted to Vulkan 1.2 may also be exposed by the extension.
    uint32_t count=0;vkEnumerateDeviceExtensionProperties(gpu,nullptr,&count,nullptr);
    VkPhysicalDeviceProperties base{};vkGetPhysicalDeviceProperties(gpu,&base);
    if(base.apiVersion>=VK_API_VERSION_1_2)properties.pNext=&driver;
    vkGetPhysicalDeviceProperties2(gpu,&properties);
    char result[2048];
    snprintf(result,sizeof result,"selection=%s\ngpu=%s\nvendor=0x%x\ndevice=0x%x\napi=%u.%u.%u\ndriver_version=%u\ndriver_name=%s\ndriver_info=%s\n",
        selection.c_str(),base.deviceName,base.vendorID,base.deviceID,VK_API_VERSION_MAJOR(base.apiVersion),
        VK_API_VERSION_MINOR(base.apiVersion),VK_API_VERSION_PATCH(base.apiVersion),base.driverVersion,driver.driverName,driver.driverInfo);
    fprintf(stderr,"[vulkan-driver] %s",result);
    if(const char* profile=getenv("TOTK_PROFILE_DIR")) {
        const auto path=std::string(profile)+"/driver-status.txt";
        if(FILE* file=fopen(path.c_str(),"w")){fputs(result,file);fclose(file);}
    }
}
