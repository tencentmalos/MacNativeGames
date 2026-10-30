// SPDX-License-Identifier: MIT
#include "performance_hint.h"
#include "../nvn_vulkan/runtime_policy.h"
#include <chrono>
#include <sstream>
#include <unistd.h>
#ifdef __ANDROID__
#include <dlfcn.h>
#include <android/performance_hint.h>
#endif

namespace mng::port {
namespace {
class PlatformHint final : public HintBackend {
public:
    bool available() override {
#ifdef __ANDROID__
        // Load at runtime so older Android versions retain the shared renderer.
        library_ = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (!library_) return false;
        get_ = reinterpret_cast<Get>(dlsym(library_, "APerformanceHint_getManager"));
        create_ = reinterpret_cast<Create>(dlsym(library_, "APerformanceHint_createSession"));
        report_ = reinterpret_cast<Report>(dlsym(library_, "APerformanceHint_reportActualWorkDuration"));
        close_ = reinterpret_cast<Close>(dlsym(library_, "APerformanceHint_closeSession"));
        if (!get_ || !create_ || !report_ || !close_) return false;
        manager_ = get_();
        return manager_ != nullptr;
#else
        return false;
#endif
    }
    void* create(int32_t tid, int64_t budget) override {
#ifdef __ANDROID__
        return create_(manager_, &tid, 1, budget);
#else
        return nullptr;
#endif
    }
    bool report(void* session, int64_t elapsed) override {
#ifdef __ANDROID__
        return report_(static_cast<APerformanceHintSession*>(session), elapsed) == 0;
#else
        return false;
#endif
    }
    void close(void* session) override {
#ifdef __ANDROID__
        close_(static_cast<APerformanceHintSession*>(session));
#endif
    }
    ~PlatformHint() override {
#ifdef __ANDROID__
        if (library_) dlclose(library_);
#endif
    }
private:
#ifdef __ANDROID__
    using Get = APerformanceHintManager* (*)();
    using Create = APerformanceHintSession* (*)(APerformanceHintManager*, const int32_t*, size_t, int64_t);
    using Report = int (*)(APerformanceHintSession*, int64_t);
    using Close = void (*)(APerformanceHintSession*);
    void* library_{};
    APerformanceHintManager* manager_{};
    Get get_{}; Create create_{}; Report report_{}; Close close_{};
#endif
};
struct Owner {
    PlatformHint backend;
    HintQueue queue{backend, mng::vulkan::policy::hint_budget_ns};
};
Owner& owner() { static Owner instance; return instance; }
uint64_t now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}
std::string performance_hint_control(int enabled) {
    auto& q = owner().queue;
    if (enabled >= 0) q.request(enabled != 0);
    HintSnapshot s;
    if (!q.snapshot(s)) return "error=emulation_session_busy\nretryable=1\n";
    std::ostringstream out;
    out << "schema=mng.performance-hint.v1\nprocess_id=" << getpid()
        << "\nrenderer_hint=" << s.requested << "\nhint_supported=" << s.supported
        << "\nhint_generation=" << s.generation << "\nhint_applied=" << s.applied
        << "\nhint_target_ns=" << mng::vulkan::policy::hint_budget_ns
        << "\nhint_translate_tid=" << s.tids[0] << "\nhint_present_tid=" << s.tids[1]
        << "\nhint_accepted=" << s.accepted << "\nhint_reported=" << s.reported
        << "\nhint_coalesced=" << s.coalesced << "\nhint_failures=" << s.failures << "\n";
    return out.str();
}
uint64_t performance_hint_begin() { return owner().queue.enabled() ? now() : 0; }
void performance_hint_end(HintLane lane, uint64_t begin) {
    if (begin) performance_hint_report(lane, now() - begin);
}
void performance_hint_report(HintLane lane, uint64_t elapsed) {
#ifdef __ANDROID__
    auto& q = owner().queue;
    if (q.enabled()) q.submit(lane, gettid(), elapsed);
#endif
}
}
