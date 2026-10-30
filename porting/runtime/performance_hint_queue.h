// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace mng::port {
enum class HintLane : unsigned { Translate, Present, Count };
// Backend methods run only on the controller worker, never with its queue lock.
struct HintBackend {
    virtual ~HintBackend() = default;
    virtual bool available() = 0;
    virtual void* create(int32_t tid, int64_t target_ns) = 0;
    virtual bool report(void* session, int64_t elapsed_ns) = 0;
    virtual void close(void* session) = 0;
};
struct HintSnapshot {
    bool requested{};
    int supported{-1};
    uint64_t generation{}, applied{}, accepted{}, reported{}, coalesced{}, failures{};
    std::array<int32_t, 2> tids{};
};
class HintQueue {
public:
    explicit HintQueue(HintBackend& backend, int64_t budget_ns)
        : backend_(backend), budget_(budget_ns) {}
    ~HintQueue() {
        { std::lock_guard lock(mutex_); stop_ = true; }
        wake_.notify_one();
        if (worker_.joinable()) worker_.join();
    }
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
    void request(bool enabled) {
        std::lock_guard lock(mutex_);
        if (state_.requested == enabled) return;
        state_.requested = enabled;
        ++state_.generation;
        pending_ = {};
        if (enabled && !worker_.joinable()) worker_ = std::thread([this] { run(); });
        enabled_.store(enabled, std::memory_order_relaxed);
        wake_.notify_one();
    }
    // At most one pending sample per lane; the producer never waits for Binder.
    void submit(HintLane lane, int32_t tid, uint64_t elapsed_ns) {
        if (!enabled() || !elapsed_ns || elapsed_ns > INT64_MAX || tid <= 0) return;
        const auto index = static_cast<unsigned>(lane);
        if (index >= pending_.size()) return;
        std::lock_guard lock(mutex_);
        if (!state_.requested) return;
        if (pending_[index].elapsed) ++state_.coalesced;
        pending_[index] = {tid, static_cast<int64_t>(elapsed_ns)};
        ++state_.accepted;
        wake_.notify_one();
    }
    bool snapshot(HintSnapshot& out) {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock()) return false;
        out = state_;
        return true;
    }
private:
    struct Sample { int32_t tid{}; int64_t elapsed{}; };
    void run() {
        std::array<void*, 2> sessions{};
        std::array<int32_t, 2> tids{};
        std::array<bool, 2> failed{};
        uint64_t generation = UINT64_MAX;
        int support = -1;
        auto close = [&] {
            for (auto& session : sessions) {
                if (session) backend_.close(session);
                session = nullptr;
            }
            tids = {}; failed = {};
        };
        for (;;) {
            std::array<Sample, 2> batch;
            bool requested; uint64_t requested_generation;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stop_ || generation != state_.generation ||
                    pending_[0].elapsed || pending_[1].elapsed; });
                if (stop_) break;
                batch = pending_; pending_ = {};
                requested = state_.requested; requested_generation = state_.generation;
            }
            if (generation != requested_generation) { close(); generation = requested_generation; }
            if (requested && support < 0) support = backend_.available() ? 1 : 0;
            uint64_t reports = 0, errors = 0;
            if (requested && support == 1) for (unsigned i = 0; i < batch.size(); ++i) {
                const auto sample = batch[i];
                if (!sample.elapsed) continue;
                if (tids[i] != sample.tid) {
                    if (sessions[i]) backend_.close(sessions[i]);
                    sessions[i] = nullptr; tids[i] = sample.tid; failed[i] = false;
                }
                // A failed session is retried only after OFF/ON or a new TID.
                if (failed[i]) continue;
                if (!sessions[i]) sessions[i] = backend_.create(sample.tid, budget_);
                if (!sessions[i] || !backend_.report(sessions[i], sample.elapsed)) {
                    if (sessions[i]) backend_.close(sessions[i]);
                    sessions[i] = nullptr; failed[i] = true; ++errors;
                } else ++reports;
            }
            {
                std::lock_guard lock(mutex_);
                state_.supported = support; state_.applied = generation;
                state_.reported += reports; state_.failures += errors;
                for (unsigned i = 0; i < sessions.size(); ++i) state_.tids[i] = sessions[i] ? tids[i] : 0;
            }
        }
        close();
    }
    HintBackend& backend_;
    const int64_t budget_;
    std::atomic<bool> enabled_{};
    std::mutex mutex_;
    std::condition_variable wake_;
    HintSnapshot state_;
    std::array<Sample, 2> pending_{};
    bool stop_{};
    std::thread worker_;
};
}
