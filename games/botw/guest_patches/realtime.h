// SPDX-License-Identifier: MIT
#pragma once
#include "port_runtime.h"
#include "frame_step.h"
namespace mng::botw {
port::PatchReceipt apply_realtime(std::span<uint8_t>, const port::Identity&, size_t page_offset);
// Owns the per-title callback. Destroy before unmapping the image, after guest stop.
class RealtimeSession {
public:
    RealtimeSession(uint8_t* image, size_t page_offset);
    ~RealtimeSession();
    RealtimeSession(const RealtimeSession&)=delete;
    RealtimeSession& operator=(const RealtimeSession&)=delete;
    int update(uint64_t now_ns, double* seconds);
private:
    uint8_t* image_;
    uint8_t* page_;
    port::FrameStep clock_;
    uint64_t report_ns_{}, frames_{};
    double real_seconds_{}, game_seconds_{};
    bool movie_{};
};
}
