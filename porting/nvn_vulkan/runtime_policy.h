// SPDX-License-Identifier: MIT
#pragma once
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mng::vulkan::policy {
struct Option { const char *key, *environment, *default_value; unsigned maximum; };
// One backend policy for every host. Platform launchers transport explicit
// overrides; they must not supply their own copies of these defaults.
inline constexpr size_t publication_capacity=4096;
// Host performance hints are runtime-only, default OFF, with the 60 Hz frame budget.
inline constexpr long long hint_budget_ns=16666667;
inline constexpr Option options[]{
    {"invalidate_fast", "MNG_INVALIDATE_FAST", "0", 1},
    {"publication_queue", "MNG_PUBLICATION_QUEUE", "0", 1},
    {"feedback_snapshot", "MNG_FEEDBACK_SNAPSHOT", "1", 1},
    {"defer_fragment_barrier", "MNG_DEFER_FRAGMENT_BARRIER", "1", 1},
    {"pipeline_memo", "DM_GFX_PIPE_MEMO", "1", 1},
    {"perf_sample", "DM_GFX_PERF_SAMPLE", "0", 65536},
    {"present_worker", "MNG_PRESENT_WORKER", "1", 1},
    {"ordered_translation", "MNG_ORDERED_TRANSLATION", "0", 1},
    {"read_preflight", "DM_GFX_READ_PREFLIGHT", "1", 1},
    {"range_preflight", "MNG_RANGE_PREFLIGHT", "1", 1},
    {"preflight_buffer_filter", "MNG_PREFLIGHT_BUFFER_FILTER", "1", 1},
    {"read_range_cache", "MNG_READ_RANGE_CACHE", "1", 1},
    {"range_flush", "MNG_RANGE_FLUSH", "1", 1},
    {"live_range_flush", "MNG_LIVE_RANGE_FLUSH", "1", 1},
    {"image_flush", "MNG_IMAGE_FLUSH", "1", 1},
    {"frontlock_diagnostics", "MNG_FRONTLOCK_DIAGNOSTICS", "0", 1},
    {"submit_batch", "MNG_SUBMIT_BATCH_OPS", "1024", 65536},
    {"upload_cached", "MNG_UPLOAD_CACHED", "0", 1},
    {"profile_startup", "MNG_PROFILE_STARTUP", "0", 1},
    {"pso_workers", "MNG_PSO_WORKERS", "0", 8},
    {"preparation_plan", "MNG_PREPARATION_PLAN", "1", 1},
    {"preparation_validate", "MNG_PREPARATION_VALIDATE", "0", 1},
    {"pipeline_early", "MNG_PIPELINE_EARLY", "1", 1},
    {"descriptor_plan", "MNG_DESCRIPTOR_PLAN", "1", 1},
    {"image_read_fast", "MNG_IMAGE_READ_FAST", "1", 1},
    {"adaptive_batch", "MNG_ADAPTIVE_BATCH", "1", 1},
};
inline bool upload_cached() {
    const char* value=std::getenv("MNG_UPLOAD_CACHED");return value && !std::strcmp(value,"1");
}
inline bool valid(const Option &option, const char *value) {
    if (!value || !*value) return false;
    if (value[0]=='0' && value[1]) return false;
    unsigned result=0;
    for (const char *c=value; *c; ++c) {
        if (*c<'0' || *c>'9' || result>option.maximum/10) return false;
        result=result*10+unsigned(*c-'0');
        if (result>option.maximum) return false;
    }
    return true;
}
inline bool apply(char *error, size_t bytes) {
    for (const auto &option : options) {
        const char *value=std::getenv(option.environment);
        if (value && !valid(option,value)) {
            if (error && bytes) std::snprintf(error,bytes,"Invalid Vulkan option %s",option.environment);
            return false;
        }
    }
    for (const auto &option : options) {
        if (!std::getenv(option.environment) && setenv(option.environment,option.default_value,0)) {
            if (error && bytes) std::snprintf(error,bytes,"Cannot initialize Vulkan option %s",option.environment);
            return false;
        }
    }
    return true;
}
}
