// SPDX-License-Identifier: MIT
#include <volk.h>
#include "../porting/nvn_vulkan/host_platform.h"
#include "../porting/nvn_vulkan/nvn_vulkan.h"
#import <QuartzCore/CAMetalLayer.h>
#import <CoreText/CoreText.h>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
extern "C" void* front_metal_layer();
extern "C" void front_frame_presented();
extern "C" void runtime_input_clear();
void* mng_host_window(){return front_metal_layer();}
const char* mng_host_surface_extension(){return VK_EXT_METAL_SURFACE_EXTENSION_NAME;}
VkResult mng_host_create_surface(VkInstance instance,VkSurfaceKHR* out){
    VkMetalSurfaceCreateInfoEXT info{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
    info.pLayer=(__bridge CAMetalLayer*)front_metal_layer();
    return vkCreateMetalSurfaceEXT(instance,&info,nullptr,out);
}
VkExtent2D mng_host_surface_extent(){CGSize size=((__bridge CAMetalLayer*)front_metal_layer()).drawableSize;return {uint32_t(size.width),uint32_t(size.height)};}
void mng_host_option(const char* name,char* out,size_t bytes){
    char key[128]="MNG_";size_t at=4;
    if(!strncmp(name,"debug.mng.",10))name+=10;
    while(*name && at+1<sizeof key){key[at++]=std::toupper(static_cast<unsigned char>(*name++));}key[at]=0;
    const char* value=getenv(key);snprintf(out,bytes,"%s",value?value:"");
}
const char* mng_host_font(){
    static const std::string path=[] {
        CTFontRef font=CTFontCreateWithName(CFSTR("PingFang SC"),18,nullptr);
        auto url=static_cast<CFURLRef>(CTFontCopyAttribute(font,kCTFontURLAttribute));
        char bytes[4096]{};if(url)CFURLGetFileSystemRepresentation(url,true,reinterpret_cast<UInt8*>(bytes),sizeof bytes);
        if(url)CFRelease(url);CFRelease(font);return std::string(bytes);
    }();return path.c_str();
}
void mng_host_capture_input(bool){runtime_input_clear();}
void mng_host_presented(){front_frame_presented();}
