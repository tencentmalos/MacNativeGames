// SPDX-License-Identifier: MIT
#include <jni.h>
#include "vulkan_driver.h"
#include "profiler.h"
#include "debugbus.h"
#include "../../porting/nvn_vulkan/runtime_policy.h"
#include "../../games/totk/runtime/system_mode.h"
extern "C" int runtime_profile_threads(char*,unsigned);
extern "C" int runtime_lane_control(int,char*,unsigned);
extern "C" void runtime_lane_present(uint64_t);
extern "C" void runtime_autoload_configure();
extern "C" int runtime_autoload_snapshot(char*,unsigned);
extern "C" int mng_nvn_resource_snapshot(char*,unsigned);
extern "C" int mng_nvn_target_snapshot(char*,unsigned);
extern "C" int runtime_input_pulse(int,unsigned,char*,unsigned);
extern "C" int mng_nvn_flush_snapshot(char*,unsigned);
extern "C" int mng_nvn_schedule(int,int,int,int,int,int,int,int,int,int,char*,unsigned);
extern "C" int runtime_core_away();
extern "C" void runtime_core_back(int*);
extern "C" void mng_nvn_set_wait_callbacks(int(*)(),void(*)(int*));
#include <android/native_window_jni.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <ctime>
#include <sys/stat.h>
#include <sys/system_properties.h>
#ifdef MNG_GAME_SERVICES
namespace mng { [[noreturn]] void startup_probe(const std::string&,const std::string&); }
extern "C" void runtime_input_key(unsigned,int);
extern "C" JNIEXPORT jboolean JNICALL Java_org_macnative_android_GameNative_key(JNIEnv*,jobject,jint code,jboolean down) {
    // Keep the Mac keyboard bindings on Android physical/ADB keyboards too.
    unsigned key;
    switch(code) {case 38:key=38;break;case 39:key=40;break;case 37:key=34;break;case 49:key=32;break;
    case 66:key=36;break;case 61:key=48;break;case 62:key=49;break;case 29:key=0;break;case 32:key=2;break;
    case 51:key=13;break;case 47:key=1;break;case 19:key=126;break;case 20:key=125;break;case 21:key=123;break;case 22:key=124;break;
    default:return false;}
    runtime_input_key(key,down);return true;
}
extern "C" void mng_android_window(ANativeWindow*);
extern "C" void mng_android_focus(bool);
extern "C" void mng_android_vsync(uint64_t,float);
extern "C" JNIEXPORT void JNICALL Java_org_macnative_android_GameNative_vsync(JNIEnv*,jobject,jlong time,jfloat hz) {mng_android_vsync(uint64_t(time),hz);}
extern "C" JNIEXPORT void JNICALL Java_org_macnative_android_GameNative_focus(JNIEnv*,jobject,jboolean focus) {mng_android_focus(focus);}
extern "C" JNIEXPORT void JNICALL Java_org_macnative_android_GameNative_stop(JNIEnv*,jobject) {
    // The native runtime owns process-lifetime guest threads. The dedicated
    // :game process is the lifetime boundary until cooperative shutdown exists.
    _exit(0);
}
extern "C" JNIEXPORT void JNICALL Java_org_macnative_android_GameNative_start(JNIEnv* env,jobject,jobject surface,jstring modules,jstring assets,jstring shaders,jstring profile,jstring driver,jstring hooks,jstring driver_dir,jstring language,jint resolution) {
    auto string=[&](jstring value){const char *p=env->GetStringUTFChars(value,nullptr);std::string s(p);env->ReleaseStringUTFChars(value,p);return s;};
    auto module_path=string(modules),root=string(assets),shader_path=string(shaders),user=string(profile);
    const auto log_path=user+"/native-game.log";
    timespec now{};clock_gettime(CLOCK_REALTIME,&now);
    const auto previous=user+"/native-game-before-"+std::to_string(now.tv_sec)+"-"+std::to_string(now.tv_nsec)+"-"+std::to_string(getpid())+".log";
    if(rename(log_path.c_str(),previous.c_str()) && errno!=ENOENT) {
        env->ThrowNew(env->FindClass("java/io/IOException"),"Cannot preserve previous native game log");return;
    }
    int fd=open(log_path.c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);
    if(fd<0){env->ThrowNew(env->FindClass("java/io/IOException"),"Cannot create native game log");return;}
    if(fd>=0){dup2(fd,STDOUT_FILENO);dup2(fd,STDERR_FILENO);close(fd);}
    setvbuf(stdout,nullptr,_IONBF,0);setvbuf(stderr,nullptr,_IONBF,0);
    if(resolution!=720 && resolution!=1080){env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),"Unsupported render resolution");return;}
    setenv("MNG_RENDER_RESOLUTION",resolution==720?"720":"1080",1);
    setenv("TOTK_OPERATION_MODE",resolution==720?"handheld":"docked",1);
    const auto *mode=runtime_system_mode();
    fprintf(stderr,"[android-boot] resolution=%dp internal_scale=1 operation_mode=%s performance_mode=%d\n",resolution,mode->name,mode->performance);
    auto kind=string(driver),hook_path=string(hooks),driver_path=string(driver_dir),locale=string(language);
    setenv("TOTK_LANG",locale.c_str(),1);setenv("TOTK_PROFILE_DIR",user.c_str(),1);
    fprintf(stderr,"[android-boot] requested driver=%s language=%s\n",kind.c_str(),locale.c_str());
    if(const char* error=mng_vulkan_initialize(kind.c_str(),hook_path.c_str(),driver_path.c_str())) {
        fprintf(stderr,"[android-boot] driver load failed: %s\n",error);
        env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),error);return;
    }
    unsetenv("MNG_HEADLESS");setenv("MNG_SHADER_DIR",shader_path.c_str(),1);
    setenv("MNG_REALTIME","1",1);
#ifndef NDEBUG
    // Keep the current module/patch identity inspectable in development APKs.
    // No per-call logging or unsolicited breakpoint is enabled.
    setenv("MNG_PORT_DEBUG","1",1);
    char input_log[PROP_VALUE_MAX]{};__system_property_get("debug.mng.input_log",input_log);
    if(!strcmp(input_log,"1"))setenv("TOTK_INPUT_LOG","1",1);
    char autoload[PROP_VALUE_MAX]{};__system_property_get("debug.mng.autoload",autoload);
    if(!strcmp(autoload,"1"))setenv("TOTK_AUTOLOAD","1",1);
    // Android properties are a transport for explicit overrides of the same
    // policy used by desktop argv/environment and standalone Vulkan probes.
    for(const auto &option:mng::vulkan::policy::options){
        char value[PROP_VALUE_MAX]{};
        const auto property=std::string("debug.mng.")+option.key;
        __system_property_get(property.c_str(),value);
        if(*value)setenv(option.environment,value,1);
    }
    // Native Guest phase tags are shared by all graphics backends.
    char tags[PROP_VALUE_MAX]{};__system_property_get("debug.mng.guest_tags",tags);
    setenv("MNG_GUEST_TAGS",!strcmp(tags,"1")?"1":"0",1);
    char realtime[PROP_VALUE_MAX]{};__system_property_get("debug.mng.realtime",realtime);
    if(!strcmp(realtime,"0") || !strcmp(realtime,"1"))setenv("MNG_REALTIME",realtime,1);
    char probe[PROP_VALUE_MAX]{};__system_property_get("debug.mng.texture_probe",probe);
    if(probe[0]){
        const auto directory=user+"/texture-probe-"+std::to_string(now.tv_sec)+"-"+std::to_string(getpid());
        if(!mkdir(directory.c_str(),0700)){
            setenv("MNG_TEXTURE_PROBE",probe,1);setenv("MNG_TEXTURE_PROBE_DIR",directory.c_str(),1);
            fprintf(stderr,"[android-boot] bounded texture upload probe %s filter=%s\n",directory.c_str(),probe);
        }
    }
#endif
    char policy_error[256]{};
    if(!mng::vulkan::policy::apply(policy_error,sizeof policy_error)){
        env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),policy_error);return;
    }
    mng::profile::initialize("MacNativeGames:game",user+"/profiler",
        "title=BOTW version=1.6.0 build_id=8e9978d50bdd20b4c8395a106c27ffde driver="+kind+" driver_path="+driver_path+
        " output_resolution="+std::to_string(resolution)+" system_mode="+mode->name+
        " operation_mode="+std::to_string(mode->operation)+" performance_mode="+std::to_string(mode->performance)+
        " internal_scale=1 read_preflight="+
        getenv("DM_GFX_READ_PREFLIGHT")+
        " range_preflight="+getenv("MNG_RANGE_PREFLIGHT")+
        " submit_batch="+getenv("MNG_SUBMIT_BATCH_OPS")+
        " range_flush="+getenv("MNG_RANGE_FLUSH")+
        " realtime="+(getenv("MNG_REALTIME")?getenv("MNG_REALTIME"):"0")+
        " pipeline_memo="+getenv("DM_GFX_PIPE_MEMO")+
        " pso_workers="+getenv("MNG_PSO_WORKERS")+
        " preparation_plan="+getenv("MNG_PREPARATION_PLAN")+
        " preparation_validate="+getenv("MNG_PREPARATION_VALIDATE")+
        " ordered_translation="+getenv("MNG_ORDERED_TRANSLATION")+
        " command_recording=independent"+
        " present_worker="+getenv("MNG_PRESENT_WORKER"));
    mng::diagnostics::setThreadSnapshot(runtime_profile_threads);
    mng::diagnostics::setLaneCallbacks(runtime_lane_control,runtime_lane_present);
    runtime_autoload_configure();
    mng::diagnostics::setAutoloadSnapshot(runtime_autoload_snapshot);
    mng::diagnostics::setResourceSnapshot(mng_nvn_resource_snapshot);
    mng::diagnostics::setTargetSnapshot(mng_nvn_target_snapshot);
    mng::diagnostics::setScheduleControl(mng_nvn_schedule);
    mng::diagnostics::setInputPulse(runtime_input_pulse);
    mng::diagnostics::setFlushSnapshot(mng_nvn_flush_snapshot);
    mng::diagnostics::setSystemModeSnapshot(runtime_system_mode_snapshot);
    mng_nvn_set_wait_callbacks(runtime_core_away,runtime_core_back);
    fprintf(stderr,"[android-boot] shader bundle %s\n",shader_path.c_str());
    setenv("TOTK_ROMFS",(root+"/romfs").c_str(),1);setenv("TOTK_AOC",(root+"/aoc").c_str(),1);
    setenv("TOTK_SHARED_FONTS",(root+"/shared-fonts").c_str(),1);setenv("TOTK_PROFILE_DIR",user.c_str(),1);
    ANativeWindow *window=ANativeWindow_fromSurface(env,surface);
    if(!window){fprintf(stderr,"Missing Android Surface\n");_exit(2);}
    mng_android_window(window);mng::startup_probe(module_path,"botw160");
}

#else
extern "C" JNIEXPORT void JNICALL Java_org_macnative_android_GameNative_start(JNIEnv* env,jobject,jobject,jstring,jstring,jstring,jstring,jstring,jstring,jstring,jstring,jint) {
    env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),"Build has no verified BOTW asset configuration");
}
extern "C" JNIEXPORT jboolean JNICALL Java_org_macnative_android_GameNative_key(JNIEnv*,jobject,jint,jboolean) {return false;}
extern "C" JNIEXPORT void JNICALL Java_org_macnative_android_GameNative_focus(JNIEnv*,jobject,jboolean) {}
extern "C" JNIEXPORT void JNICALL Java_org_macnative_android_GameNative_vsync(JNIEnv*,jobject,jlong,jfloat) {}
extern "C" JNIEXPORT void JNICALL Java_org_macnative_android_GameNative_stop(JNIEnv*,jobject) {_exit(0);}
#endif
extern "C" JNIEXPORT jboolean JNICALL Java_org_macnative_android_GameNative_available(JNIEnv*,jobject) {
#ifdef MNG_GAME_SERVICES
    return true;
#else
    return false;
#endif
}
