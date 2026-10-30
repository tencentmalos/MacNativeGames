// SPDX-License-Identifier: GPL-2.0-or-later
#include "overlay.h"
#include "input.h"
#include <jni.h>
#ifdef MNG_GAME_SERVICES
extern "C" void runtime_input_clear();
#endif
#define JNI(name) Java_org_macnative_android_OverlayNative_##name
extern "C" JNIEXPORT void JNICALL JNI(touch)(JNIEnv*,jobject,jint phase,jfloat x,jfloat y) {mng::overlay::shared().pointer(phase,x,y);}
extern "C" JNIEXPORT jboolean JNICALL JNI(key)(JNIEnv*,jobject,jint code,jboolean down) {
    const bool was=mng::overlay::shared().capturesInput();const bool result=mng::overlay::shared().key(code,down);
    if(!was&&mng::overlay::shared().capturesInput()) {
        mng::input.block(true);
#ifdef MNG_GAME_SERVICES
        runtime_input_clear();
#endif
    }
    return result;
}
extern "C" JNIEXPORT void JNICALL JNI(cancel)(JNIEnv*,jobject) {mng::overlay::shared().cancelInput();}
extern "C" JNIEXPORT void JNICALL JNI(battery)(JNIEnv*,jobject,jint level,jlong current,jlong charge,jint voltage,jint temperature,jboolean charging) {mng::overlay::battery(level,current,charge,voltage,temperature,charging);}
