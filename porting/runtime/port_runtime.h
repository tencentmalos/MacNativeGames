// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "port_debug.h"

namespace mng::port {
std::string sha256(std::span<const uint8_t> bytes);
struct Identity {
    std::string profile, platform, module, build_id, source_sha256, image_sha256;
};
enum class PatchKind { Compatibility, Feature, Diagnostic };
struct Patch {
    std::string id;
    PatchKind kind;
    Identity identity;
    size_t offset;
    std::vector<uint8_t> expected, replacement;
    std::string reason, evidence;
};
struct PatchReceipt {
    Identity identity;
    std::string input_sha256, output_sha256;
    struct Entry {
        std::string id;
        PatchKind kind;
        size_t module_offset, size;
        std::string expected_sha256, replacement_sha256, reason, evidence;
    };
    std::vector<Entry> applied;
};
// Private RW staging only, before relocation/sealing/execution. Verify the
// whole image and every selected patch before the first write. An empty list
// enables nothing. Feature/diagnostic patches additionally need their category
// explicitly allowed. No protection changes or live code writes occur here.
PatchReceipt apply_staged(std::span<uint8_t> image, const Identity& identity,
    std::span<const Patch> patches, std::span<const std::string> selected,
    bool allow_features=false, bool allow_diagnostics=false);

// One loader owner per process; a second owner is rejected, not substituted.
// Payload publication is bounded and opt-in. Generation survives unload/reload
// within the process. Native debugger target identity handles process reuse.
class DebugSession {
public:
    explicit DebugSession(bool enabled);
    ~DebugSession();
    DebugSession(const DebugSession&)=delete;
    DebugSession& operator=(const DebugSession&)=delete;
    uint64_t generation() const { return generation_; }
    void publish(std::string payload);
private:
    uint64_t generation_{};
};
}
