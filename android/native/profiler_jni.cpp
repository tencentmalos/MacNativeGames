// SPDX-License-Identifier: MIT
#include <jni.h>
#include "debugbus.h"
#include <exception>
extern "C" JNIEXPORT jstring JNICALL Java_org_macnative_android_PortProfiler_command(JNIEnv* env,jobject,jstring request) {
    if(!request)return env->NewStringUTF("error=missing_command\n");
    const char* chars=env->GetStringUTFChars(request,nullptr);
    if(!chars)return nullptr;
    std::string text(chars);env->ReleaseStringUTFChars(request,chars);
    try {return env->NewStringUTF(mng::diagnostics::command(text).c_str());}
    catch(const std::exception& error){return env->NewStringUTF((std::string("error=")+error.what()+"\n").c_str());}
}
