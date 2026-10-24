// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>

namespace mng::port {
// Horizon ABI facts are cross-checked against the existing Mac host and
// Citron svc_types.h/svc_results.h. No emulator memory manager is embedded.
struct MemoryInfo {
    uint64_t base, size;
    uint32_t state, attribute, permission, ipc_count{}, device_count{}, padding{};
};
static_assert(sizeof(MemoryInfo) == 40);
struct MemoryConfig {
    uint64_t stack_size = 256ull << 20;
    uint64_t heap_capacity = 4ull << 30;
    uint64_t alias_size = 64ull << 30;
    uint64_t pool_size = 0xcd500000;
    uint64_t image_size = 0;
};
class GuestMemory {
  public:
    explicit GuestMemory(MemoryConfig config = {});
    ~GuestMemory();
    GuestMemory(const GuestMemory &) = delete;
    GuestMemory &operator=(const GuestMemory &) = delete;
    uint64_t base() const { return base_; }
    uint64_t size() const { return size_; }
    uint64_t stack() const { return base_; }
    uint64_t heap() const { return base_ + config_.stack_size; }
    uint64_t alias() const { return heap() + config_.heap_capacity; }
    uint64_t heap_size() const;
    uint64_t alias_bytes() const;
    // Register loader-owned mappings. They remain owned by the caller and
    // cannot be changed by this class's heap/physical-memory operations.
    void register_mapping(uint64_t base, uint64_t size, uint32_t state, uint32_t permission);
    MemoryInfo query(uint64_t address) const;
    bool accessible(uint64_t address, uint64_t size, uint32_t permission) const;
    std::optional<uint64_t> info(uint32_t id) const;
    uint32_t set_heap_size(uint64_t size);
    uint32_t map_physical(uint64_t address, uint64_t size, bool unmap = false);
    uint32_t map_stack(uint64_t destination, uint64_t source, uint64_t size, bool unmap = false);
    uint32_t set_permission(uint64_t address, uint64_t size, uint32_t permission);

  private:
    struct Segment {
        uint64_t end;
        uint32_t state, attribute, permission;
    };
    using Segments = std::map<uint64_t, Segment>;
    MemoryConfig config_;
    uint64_t base_{}, size_{}, page_{}, heap_size_{}, alias_bytes_{};
    mutable std::mutex mutex_;
    Segments segments_;
    struct StackMapping {
        uint64_t source, size;
    };
    std::map<uint64_t, StackMapping> stack_mappings_;
    MemoryInfo query_locked(uint64_t address) const;
    void replace(uint64_t base, uint64_t size, uint32_t state, uint32_t permission,
                 uint32_t attribute = 0);
    void protect_logical(uint64_t address, uint64_t size);
    void discard(uint64_t address, uint64_t size);
    void protect(uint64_t address, uint64_t size, int permission);
};
} // namespace mng::port
