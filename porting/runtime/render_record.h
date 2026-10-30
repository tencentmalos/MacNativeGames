// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

namespace mng::port {
// Called by one serialized rendering owner. The backend owns actual command /
// resource capture. This sidecar is an index, never a standalone replay stream.
class RenderRecord {
public:
    struct Backend {
        std::function<bool(bool begin)> capture;
        std::function<uint64_t()> milliseconds;
    };
    RenderRecord(std::string directory, std::string run, std::string identity, Backend backend);
    ~RenderRecord();
    RenderRecord(const RenderRecord&)=delete;
    RenderRecord& operator=(const RenderRecord&)=delete;
    bool arm(uint64_t request, uint32_t frames=1, uint64_t max_bytes=64u<<20,
             uint64_t max_events=250000, uint64_t timeout_ms=60000);
    void present(uint64_t completed_frame);
    // addresses identify translated host command storage, not original guest PCs.
    const char* action(uint64_t submission, uint64_t address, uint32_t opcode,
                       uint32_t bytes, const char* name, const char* detail="");
    void abort(const char* reason);
    bool active() const { return file_!=nullptr; }
    bool armed() const { return armed_; }
    const std::string& error() const { return error_; }
    const std::string& path() const { return path_; }
private:
    bool write(const std::string&, bool terminal=false);
    void finish(bool complete, const char* reason);
    std::string directory_,run_,identity_,path_,error_,label_;
    Backend backend_;
    FILE* file_{};
    bool armed_{},backend_active_{};
    uint64_t request_{},frame_{},first_frame_{},bytes_{},events_{},max_bytes_{},max_events_{},deadline_{},timeout_{};
    uint32_t frames_{},done_{};
};
std::string record_json_string(const std::string& value);
}
