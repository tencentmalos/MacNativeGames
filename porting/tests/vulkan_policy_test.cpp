// SPDX-License-Identifier: MIT
#include "../nvn_vulkan/runtime_policy.h"
#include <cassert>
#include <initializer_list>
int main() {
    using namespace mng::vulkan::policy;
    for (const auto &option:options) unsetenv(option.environment);
    char error[256]{};
    assert(apply(error,sizeof error));
    assert(!strcmp(getenv("MNG_FEEDBACK_SNAPSHOT"),"1"));
    assert(!strcmp(getenv("MNG_DEFER_FRAGMENT_BARRIER"),"1"));
    assert(!strcmp(getenv("DM_GFX_PERF_SAMPLE"),"0"));
    assert(!strcmp(getenv("MNG_PREPARATION_PLAN"),"1"));
    assert(!strcmp(getenv("MNG_PREPARATION_VALIDATE"),"0"));
    assert(!strcmp(getenv("MNG_ORDERED_TRANSLATION"),"0"));
    assert(!strcmp(getenv("MNG_INVALIDATE_FAST"),"0"));
    for(const auto *name:{"MNG_INVALIDATE_FAST","MNG_PREPARATION_PLAN","MNG_PREPARATION_VALIDATE","MNG_ORDERED_TRANSLATION"}){
        setenv(name,"2",1);assert(!apply(error,sizeof error));assert(strstr(error,name));
        setenv(name,"1",1);assert(apply(error,sizeof error));
        setenv(name,"0",1);assert(apply(error,sizeof error));
    }
    setenv("MNG_FEEDBACK_SNAPSHOT","0",1);
    setenv("MNG_SUBMIT_BATCH_OPS","0",1);
    assert(apply(error,sizeof error));
    assert(!strcmp(getenv("MNG_FEEDBACK_SNAPSHOT"),"0"));
    assert(!strcmp(getenv("MNG_SUBMIT_BATCH_OPS"),"0"));
    for (const char *bad:{"-1","65537","999999999999999999999","1junk","","01"}) {
        setenv("MNG_SUBMIT_BATCH_OPS",bad,1);
        assert(!apply(error,sizeof error));
        assert(strstr(error,"MNG_SUBMIT_BATCH_OPS"));
    }
    setenv("MNG_SUBMIT_BATCH_OPS","65536",1);
    assert(apply(error,sizeof error));
    setenv("MNG_PSO_WORKERS","8",1);
    assert(apply(error,sizeof error));
    setenv("MNG_PSO_WORKERS","9",1);
    assert(!apply(error,sizeof error));
    setenv("MNG_PSO_WORKERS","0",1);
    setenv("MNG_PROFILE_STARTUP","2",1);
    assert(!apply(error,sizeof error));
    setenv("MNG_PROFILE_STARTUP","0",1);
    setenv("MNG_DEFER_FRAGMENT_BARRIER","2",1);
    assert(!apply(error,sizeof error));
    puts("Shared Vulkan defaults, explicit overrides and invalid-input rejection passed");
}
