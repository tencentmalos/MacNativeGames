// SPDX-License-Identifier: GPL-2.0-or-later
#include "input.h"
#include "platform.h"
#include "guest_runtime.h"
#include "nvn_vulkan.h"
#include <jni.h>
#include <cmath>
#include <chrono>
#include <thread>
#include <cstdio>
using namespace spatial::input;
#define JNI(name) Java_org_macnative_android_Native_##name
extern "C" JNIEXPORT jstring JNICALL JNI(modules)(JNIEnv* e,jobject,jstring directory,jstring title,jboolean execute) {
    if(!directory || !title)return e->NewStringUTF("{\"status\":\"fail\",\"error\":\"Missing module path or title\"}");
    const char* d=e->GetStringUTFChars(directory,nullptr);if(!d)return nullptr;
    std::string path(d);e->ReleaseStringUTFChars(directory,d);
    const char* t=e->GetStringUTFChars(title,nullptr);if(!t)return nullptr;
    std::string profile(t);e->ReleaseStringUTFChars(title,t);
    const auto report=execute?mng::probe_modules(path,profile):mng::validate_modules(path,profile);
    return e->NewStringUTF(report.c_str());
}
extern "C" JNIEXPORT void JNICALL JNI(configureInput)(JNIEnv*,jobject,jboolean swap_ab) {mng::input.configure(swap_ab);}
extern "C" JNIEXPORT jlong JNICALL JNI(begin)(JNIEnv*,jobject) { return mng::input.begin(); }
extern "C" JNIEXPORT void JNICALL JNI(end)(JNIEnv*,jobject,jlong token) { mng::input.end(token); }
template<class T,class A> std::vector<T> array(JNIEnv* env,A a);
template<> std::vector<jint> array(JNIEnv* e,jintArray a) {
    if(!a) return {};auto n=e->GetArrayLength(a);if(n>1024) return {};
    std::vector<jint> v(n);e->GetIntArrayRegion(a,0,n,v.data());return v;
}
template<> std::vector<jfloat> array(JNIEnv* e,jfloatArray a) {
    if(!a) return {};auto n=e->GetArrayLength(a);if(n>1024) return {};
    std::vector<jfloat> v(n);e->GetFloatArrayRegion(a,0,n,v.data());return v;
}
extern "C" JNIEXPORT jboolean JNICALL JNI(device)(JNIEnv* e,jobject,jlong token,jlong id,jlong epoch,jstring profile,jintArray buttons,jintArray axes,jfloatArray ranges) {
    auto b=array<jint>(e,buttons),a=array<jint>(e,axes);auto r=array<jfloat>(e,ranges);
    if(!profile || r.size()!=a.size()*3) return false;
    DeviceCapabilities caps;
    for(int v:b) {if(v<0 || v>=int(Button::Count)) return false;caps.buttons.push_back(Button(v));}
    for(size_t i=0;i<a.size();i++) {if(a[i]<0 || a[i]>=int(Axis::Count)) return false;caps.axes.push_back({Axis(a[i]),r[i*3],r[i*3+1],r[i*3+2]});}
    const char* text=e->GetStringUTFChars(profile,nullptr);if(!text) return false;
    std::string key(text);e->ReleaseStringUTFChars(profile,text);
    return mng::input.device(token,id,epoch,key,caps);
}
extern "C" JNIEXPORT jboolean JNICALL JNI(packet)(JNIEnv* e,jobject,jlong token,jlong id,jlong epoch,jlong seq,jint control,jintArray kinds,jintArray codes,jfloatArray values) {
    auto k=array<jint>(e,kinds),c=array<jint>(e,codes);auto v=array<jfloat>(e,values);
    if(k.size()!=c.size() || k.size()!=v.size() || k.size()>kMaxEventsPerPacket || control<0 || control>int(ControlEvent::Reset)) return false;
    std::vector<InputEvent> events;
    for(size_t i=0;i<k.size();i++) {
        InputEvent event;
        if(k[i]==0 && c[i]>=0 && c[i]<int(Button::Count)) {event.kind=InputEvent::Kind::ButtonState;event.button=Button(c[i]);event.pressed=v[i]!=0;}
        else if(k[i]==1 && c[i]>=0 && c[i]<int(Axis::Count)) {event.kind=InputEvent::Kind::AxisValue;event.axis=Axis(c[i]);event.raw_value=v[i];}
        else return false;
        events.push_back(event);
    }
    return mng::input.packet(token,id,epoch,seq,ControlEvent(control),std::move(events));
}
extern "C" JNIEXPORT jstring JNICALL JNI(status)(JNIEnv* e,jobject) {
    auto p=mng::input.read();char s[256];
    snprintf(s,sizeof(s),"Controller: %s\nSwitch buttons: 0x%llx\nL: %d, %d    R: %d, %d",p.connected?"connected":"disconnected",(unsigned long long)p.buttons,p.lx,p.ly,p.rx,p.ry);
    return e->NewStringUTF(s);
}
extern "C" JNIEXPORT jstring JNICALL JNI(audioProbe)(JNIEnv* e,jobject) {
    struct Tone { unsigned frame{}; } tone;
    auto pull=[](void* ctx,float* pcm,unsigned frames) {
        auto& t=*static_cast<Tone*>(ctx);
        for(unsigned i=0;i<frames;i++,t.frame++) pcm[i*2]=pcm[i*2+1]=0.04f*std::sin(t.frame*440.0f*6.2831853f/48000);
    };
    void* stream=mng_audio_open(48000,pull,&tone);
    if(!stream) return e->NewStringUTF("Audio device open failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    auto consumed=mng_audio_consumed(stream);
    mng_audio_close(stream);
    char result[100];snprintf(result,sizeof result,"Foundation audio: supplied=%u consumed_frames=%llu",tone.frame,(unsigned long long)consumed);
    return e->NewStringUTF(result);
}

extern "C" JNIEXPORT jstring JNICALL JNI(vulkanProbe)(JNIEnv* e,jobject,jstring directory) {
    if(!directory)return e->NewStringUTF("FAIL: Missing shader-cache directory");
    const char* text=e->GetStringUTFChars(directory,nullptr);if(!text)return nullptr;
    std::string path(text);e->ReleaseStringUTFChars(directory,text);
    char report[2048]{};mng_nvn_self_test(path.c_str(),report,sizeof(report));
    return e->NewStringUTF(report);
}
