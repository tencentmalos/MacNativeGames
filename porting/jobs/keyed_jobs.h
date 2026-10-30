// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mng::jobs {
// A session-owned, bounded preparation cache. Results stay owned by the build
// callback's consumer. Workers never publish draws or touch guest/global state.
class KeyedJobs {
public:
    struct Stats {
        uint64_t submitted=0, deduplicated=0, built=0, failed=0, cancelled=0;
        uint64_t active=0, peak=0, claimed=0, waits=0, rejected=0, bytes=0;
    };
    explicit KeyedJobs(unsigned workers, size_t max_tasks=4096, size_t max_bytes=64u<<20);
    ~KeyedJobs();
    KeyedJobs(const KeyedJobs&)=delete;
    KeyedJobs& operator=(const KeyedJobs&)=delete;
    // Own all callback inputs. A duplicate key never executes a second build.
    bool enqueue(std::string key, size_t input_bytes, std::function<bool()> build);
    // A queued task can be claimed by the ordered consumer; an in-progress
    // task is joined. False means absent/failed/cancelled, allowing fallback.
    bool acquire(const std::string& key);
    Stats stats() const;
    // Owner must close submission before stop. Cancels queued tasks, joins
    // running callbacks, and only then lets the renderer destroy dependencies.
    void stop();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
