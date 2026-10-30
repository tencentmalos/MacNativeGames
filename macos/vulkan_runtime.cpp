// SPDX-License-Identifier: MIT
#include "../porting/nvn_vulkan/nvn_vulkan.h"
#include "../porting/profiler/debugbus.h"
#include "../porting/runtime/present_period.h"
#include <mach/mach.h>
#include "../games/totk/third_party/d2r_gfx/gfx/nvn/include/mach_vm_compat.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
extern "C" {
[[noreturn]] void runtime_fail(const char*);
uint64_t runtime_now_ns();
int runtime_core_away();void runtime_core_back(int*);
void runtime_frame_step_update(uint64_t);
int runtime_frame_step_sample(double*);
int runtime_game_fps_limit();
void mng_nvn_set_wait_callbacks(int(*)(),void(*)(int*));
void mng_nvn_flush();
uint64_t runtime_invoke_guest(void* fn,const uint64_t* args,uint32_t count){
    // Darwin guest code already uses the host integer ABI. The existing Mac
    // HLE stack switch preserves the guest stack/TLS around its callbacks.
    if(count>8)runtime_fail("Vulkan guest callback argument limit");
    uint64_t values[8]{};for(unsigned i=0;i<count;++i)values[i]=args[i];
    return reinterpret_cast<uint64_t(*)(uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t)>(fn)
        (values[0],values[1],values[2],values[3],values[4],values[5],values[6],values[7]);
}
int mng_guest_memory_writable(uint64_t address,size_t bytes){
    if(!address || bytes>UINT64_MAX-address)return 0;
    const uint64_t end=address+bytes;
    while(address<end){mach_vm_address_t base=address;mach_vm_size_t size{};vm_region_basic_info_data_64_t info{};
        mach_msg_type_number_t count=VM_REGION_BASIC_INFO_COUNT_64;mach_port_t object{};
        auto result=mach_vm_region(mach_task_self(),&base,&size,VM_REGION_BASIC_INFO_64,reinterpret_cast<vm_region_info_t>(&info),&count,&object);
        if(object)mach_port_deallocate(mach_task_self(),object);
        if(result!=KERN_SUCCESS || base>address || !size || size>UINT64_MAX-base || !(info.protection&VM_PROT_WRITE))return 0;
        address=base+size;
    }return 1;
}
int mng_game_frame_step(uint64_t now,double* seconds){runtime_frame_step_update(now);if(!runtime_frame_step_sample(seconds))return 0;return runtime_game_fps_limit()==30?2:1;}
uint64_t mng_game_present_period(int interval,int policy) {
    static const int hz=[] {
        const char* value=getenv("MNG_PRESENT_HZ");if(!value)return 0;
        char* end{};long value_hz=strtol(value,&end,10);
        if(!*value || *end || value_hz<30 || value_hz>240)runtime_fail("MNG_PRESENT_HZ must be 30..240");
        fprintf(stderr,"[native-present] gameplay_ceiling_hz=%ld movie_hz=30 clock=own\n",value_hz);
        return int(value_hz);
    }();
    return mng::port::present_period(interval,policy,hz);
}
int runtime_gfx_fault(void* addr,int,void*){return xbg_wtrack_fault(addr);}
void dm_guest_write_prepare(void* addr,size_t bytes){xbg_wtrack_prepare(addr,bytes);}
uint64_t dm_nvn_frame_count(){xbg_game_stats stats{};xbg_game_get_stats(&stats);return stats.presents;}
int mng_nvn_resource_snapshot(char*,unsigned);
int mng_nvn_target_snapshot(char*,unsigned);
int mng_nvn_flush_snapshot(char*,unsigned);
int mng_nvn_schedule(int,int,int,int,int,int,int,int,int,int,char*,unsigned);
void mng_macos_vulkan_ready(){
    mng_nvn_set_wait_callbacks(runtime_core_away,runtime_core_back);
    mng::diagnostics::setResourceSnapshot(mng_nvn_resource_snapshot);
    mng::diagnostics::setTargetSnapshot(mng_nvn_target_snapshot);
    mng::diagnostics::setFlushSnapshot(mng_nvn_flush_snapshot);
    mng::diagnostics::setScheduleControl(mng_nvn_schedule);
}
// Existing Metal-only developer controls must be visible as unsupported rather
// than silently changing the Vulkan path or pretending a capture succeeded.
static void unsupported(const char* name){fprintf(stderr,"[mng-vulkan] unsupported Metal control: %s\n",name);}
int mfx_enabled(){return 0;}
int mfxup_enabled(){return 0;}
int mfxup_output_enabled(){return 0;}
int taa_enabled(){return 0;}
int taa_effective(){return 0;}
void taa_toggle(){unsupported("MetalFX temporal AA");}
int dm_gfx_fps_now(int* generated){if(generated)*generated=0;return -1;}
void dm_gfx_set_flag(const char* name,int){unsupported(name);}
void dm_gfx_shot_at(long,const char*){unsupported("shot");}
void dm_gfx_passdump_next(long){unsupported("passdump");}
void dm_gfx_anomdump(){unsupported("anomdump");}
void dm_gfx_querylog_next(long){unsupported("querylog");}
void dm_gfx_texdump_next(const char*){unsupported("texdump");}
void dm_gfx_window_shot(const char*){unsupported("window_shot");}
void dm_gfx_ashot(const char*){unsupported("ashot");}
void runtime_gfx_probe(int){runtime_fail("Use mng_vulkan_probe for Vulkan conformance");}
void runtime_vpool_probe(){runtime_fail("Use mng_vulkan_probe for Vulkan pool conformance");}
void runtime_gpu_path_probe(const char*){runtime_fail("Use mng_vulkan_probe; Metal probe was requested on Vulkan");}
}
