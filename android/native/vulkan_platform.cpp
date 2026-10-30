// SPDX-License-Identifier: MIT
#include "../../porting/nvn_vulkan/host_platform.h"
#include "input.h"
#include <android/native_window.h>
#include <sys/system_properties.h>
#include <cstdio>
extern "C" ANativeWindow* mng_android_get_window();
extern "C" void runtime_input_clear();
void* mng_host_window(){return mng_android_get_window();}
const char* mng_host_surface_extension(){return VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;}
VkResult mng_host_create_surface(VkInstance instance,VkSurfaceKHR* out){
    VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};info.window=mng_android_get_window();
    return vkCreateAndroidSurfaceKHR(instance,&info,nullptr,out);
}
VkExtent2D mng_host_surface_extent(){auto* window=mng_android_get_window();return {uint32_t(ANativeWindow_getWidth(window)),uint32_t(ANativeWindow_getHeight(window))};}
void mng_host_option(const char* name,char* out,size_t bytes){char value[PROP_VALUE_MAX]{};__system_property_get(name,value);snprintf(out,bytes,"%s",value);}
const char* mng_host_font(){return "/system/fonts/NotoSansCJK-Regular.ttc";}
void mng_host_capture_input(bool capture){mng::input.block(capture);runtime_input_clear();}
void mng_host_presented(){}
