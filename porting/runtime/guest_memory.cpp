// SPDX-License-Identifier: MIT
#include "guest_memory.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace mng::port {
namespace {
constexpr uint64_t limit = 1ull << 39, guest_page = 4096, heap_alignment = 2ull << 20;
constexpr uint32_t error(unsigned value) { return (value << 9) | 1; }
void need(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
bool contains(uint64_t base, uint64_t size, uint64_t address, uint64_t bytes) {
    return address >= base && bytes <= size && address - base <= size - bytes;
}
} // namespace
GuestMemory::GuestMemory(MemoryConfig config) : config_(config) {
    page_ = uint64_t(sysconf(_SC_PAGESIZE));
    need(page_ == 4096 || page_ == 16384, "Unsupported guest-memory host page size");
    need(config.stack_size && config.heap_capacity && config.alias_size &&
             config.stack_size < limit / 4 && config.heap_capacity < limit / 4 &&
             config.alias_size < limit / 4 && !(config.stack_size % heap_alignment) &&
             !(config.heap_capacity % heap_alignment) && !(config.alias_size % heap_alignment) &&
             config.image_size <= config.pool_size,
         "Invalid guest-memory configuration");
    size_ = config.stack_size + config.heap_capacity + config.alias_size;
    // A hint never overwrites a foreign mapping. Reject relocated hints; this
    // works without relying on MAP_FIXED_NOREPLACE support in old kernels.
    for (uint64_t at = 8ull << 30; size_ <= limit - at; at += 512ull << 20) {
        void *p = mmap(reinterpret_cast<void *>(at), size_, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
                       -1, 0);
        if (p == reinterpret_cast<void *>(at)) {
            base_ = at;
            break;
        }
        if (p != MAP_FAILED)
            munmap(p, size_);
    }
    need(base_ != 0, "Cannot reserve guest memory below 2^39");
    try {
        segments_.emplace(base_, Segment{base_ + size_, 0, 0, 0});
    } catch (...) {
        munmap(reinterpret_cast<void *>(base_), size_);
        throw;
    }
}
GuestMemory::~GuestMemory() {
    if (base_)
        munmap(reinterpret_cast<void *>(base_), size_);
}
void GuestMemory::register_mapping(uint64_t base, uint64_t size, uint32_t state,
                                   uint32_t permission) {
    std::lock_guard lock(mutex_);
    need(size && base < limit && size <= limit - base && !((base | size) % guest_page),
         "Invalid registered guest range");
    auto next = segments_.lower_bound(base);
    need(next == segments_.end() || base + size <= next->first,
         "Registered guest mappings overlap");
    need(next == segments_.begin() || std::prev(next)->second.end <= base,
         "Registered guest mappings overlap");
    segments_.emplace(base, Segment{base + size, state, 0, permission});
}
MemoryInfo GuestMemory::query_locked(uint64_t address) const {
    if (address >= limit)
        return {limit, uint64_t(0) - limit, 0x10, 0, 0};
    auto next = segments_.upper_bound(address);
    uint64_t low = 0;
    if (next != segments_.begin()) {
        auto previous = std::prev(next);
        if (address < previous->second.end) {
            const auto &s = previous->second;
            return {previous->first, s.end - previous->first, s.state, s.attribute, s.permission};
        }
        low = previous->second.end;
    }
    // Only our reserved arena is free guest VA. Everything outside registered
    // ranges is inaccessible; never expose an arbitrary host mapping as free.
    return {low, (next == segments_.end() ? limit : next->first) - low, 0x10, 0, 0};
}
MemoryInfo GuestMemory::query(uint64_t address) const {
    std::lock_guard lock(mutex_);
    return query_locked(address);
}
bool GuestMemory::accessible(uint64_t address, uint64_t size, uint32_t permission) const {
    std::lock_guard lock(mutex_);
    if (!size || address >= limit || size > limit - address)
        return false;
    const auto end = address + size;
    while (address < end) {
        const auto region = query_locked(address);
        if ((region.permission & permission) != permission || !region.state || region.state == 0x10)
            return false;
        address = std::min(end, region.base + region.size);
    }
    return true;
}
uint64_t GuestMemory::heap_size() const {
    std::lock_guard lock(mutex_);
    return heap_size_;
}
uint64_t GuestMemory::alias_bytes() const {
    std::lock_guard lock(mutex_);
    return alias_bytes_;
}
std::optional<uint64_t> GuestMemory::info(uint32_t id) const {
    std::lock_guard lock(mutex_);
    switch (id) {
    case 2:
        return alias();
    case 3:
        return config_.alias_size;
    case 4:
        return heap();
    case 5:
        return config_.heap_capacity;
    case 6:
    case 21:
        return config_.pool_size;
    case 7:
    case 22:
        return config_.image_size + heap_size_ + alias_bytes_;
    case 12:
        return base_;
    case 13:
        return size_;
    case 14:
        return stack();
    case 15:
        return config_.stack_size;
    default:
        return std::nullopt;
    }
}
void GuestMemory::replace(uint64_t base, uint64_t size, uint32_t state, uint32_t permission,
                          uint32_t attribute) {
    // Allocate the new logical table before changing physical protections.
    // The callers commit/decommit only after this succeeds.
    auto updated = segments_;
    const auto end = base + size;
    auto first = updated.upper_bound(base);
    need(first != updated.begin(), "Guest-memory range not reserved");
    --first;
    if (first->first < base) {
        const auto original = first->second;
        first->second.end = base;
        first = updated.emplace(base, original).first;
    }
    auto last = updated.lower_bound(end);
    if (last != updated.begin()) {
        auto before = std::prev(last);
        if (before->second.end > end)
            updated.emplace(end, before->second);
    }
    updated.erase(first, updated.lower_bound(end));
    updated.emplace(base, Segment{end, state, attribute, permission});
    for (auto it = updated.begin(); it != updated.end();) {
        auto next = std::next(it);
        if (next != updated.end() && it->second.end == next->first &&
            it->second.state == next->second.state &&
            it->second.permission == next->second.permission &&
            it->second.attribute == next->second.attribute) {
            it->second.end = next->second.end;
            updated.erase(next);
        } else
            ++it;
    }
    segments_.swap(updated);
}
void GuestMemory::protect(uint64_t address, uint64_t size, int permission) {
    if (size)
        need(mprotect(reinterpret_cast<void *>(address), size, permission) == 0,
             "Guest-memory protection failed");
}
void GuestMemory::protect_logical(uint64_t address, uint64_t size) {
    const auto low = address & ~(page_ - 1), high = (address + size + page_ - 1) & ~(page_ - 1);
    // A 16 KiB host page cannot protect its four 4 KiB guest pages separately.
    // Keep the union at host level, but exact permissions in QueryMemory.
    for (auto at = low; at < high; at += page_) {
        uint32_t permission = 0;
        for (auto sub = at; sub < at + page_; sub += guest_page)
            permission |= query_locked(sub).permission;
        protect(at, page_, int(permission));
    }
}
void GuestMemory::discard(uint64_t address, uint64_t size) {
    // Called after the logical range becomes free. Whole unused host pages are
    // discarded lazily. A partial 16 KiB page retains adjacent guest data, and
    // only its freed 4 KiB subpages are cleared before they can be remapped.
    const auto end = address + size;
    const auto low = address & ~(page_ - 1), high = (end + page_ - 1) & ~(page_ - 1);
    auto reset = [&](uint64_t page, uint64_t bytes) {
        need(contains(base_, size_, page, bytes), "Discard outside owned guest reservation");
        // MAP_FIXED replaces only pages in our still-owned reservation. Unlike
        // Darwin MADV_DONTNEED, a fresh anonymous mapping guarantees zero fill.
        need(mmap(reinterpret_cast<void *>(page), bytes, PROT_NONE,
                  MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == reinterpret_cast<void *>(page),
             "Guest-memory discard failed");
    };
    auto boundary = [&](uint64_t page) {
        uint32_t permission = 0;
        for (uint64_t at = page; at < page + page_; at += guest_page)
            permission |= query_locked(at).permission;
        if (!permission) {
            reset(page, page_);
        } else {
            protect(page, page_, PROT_READ | PROT_WRITE);
            const auto begin = std::max(page, address), finish = std::min(page + page_, end);
            memset(reinterpret_cast<void *>(begin), 0, finish - begin);
            protect(page, page_, int(permission));
        }
    };
    if (low + page_ == high) {
        boundary(low);
        return;
    }
    boundary(low);
    boundary(high - page_);
    if (high - low > 2 * page_) {
        reset(low + page_, high - low - 2 * page_);
    }
}
uint32_t GuestMemory::set_heap_size(uint64_t size) {
    std::lock_guard lock(mutex_);
    if (size % heap_alignment || size > config_.heap_capacity)
        return error(101);
    if (size > config_.pool_size - config_.image_size - alias_bytes_)
        return error(132);
    if (size == heap_size_)
        return 0;
    const auto previous = heap_size_;
    if (size < previous)
        for (auto at = heap() + size; at < heap() + previous;) {
            const auto region = query_locked(at);
            if (region.attribute)
                return error(106);
            at = region.base + region.size;
        }
    if (size > previous) {
        replace(heap() + previous, size - previous, 5, 3);
        protect(heap() + previous, size - previous, PROT_READ | PROT_WRITE);
    } else {
        replace(heap() + size, previous - size, 0, 0);
        discard(heap() + size, previous - size);
    }
    heap_size_ = size;
    return 0;
}
uint32_t GuestMemory::map_physical(uint64_t address, uint64_t size, bool unmap) {
    std::lock_guard lock(mutex_);
    if (address % guest_page)
        return error(102);
    if (!size || size % guest_page)
        return error(101);
    if (!contains(alias(), config_.alias_size, address, size))
        return error(110);
    uint64_t allocated = 0;
    for (auto at = address; at < address + size;) {
        const auto region = query_locked(at);
        if ((region.state != 0 && region.state != 5) || region.attribute ||
            (region.state == 5 && region.permission != 3))
            return error(106);
        const auto finish = std::min(address + size, region.base + region.size);
        if (region.state)
            allocated += finish - at;
        at = finish;
    }
    if (!unmap &&
        size - allocated > config_.pool_size - config_.image_size - heap_size_ - alias_bytes_)
        return error(132);
    replace(address, size, unmap ? 0 : 5, unmap ? 0 : 3);
    if (unmap) {
        discard(address, size);
        alias_bytes_ -= allocated;
    } else {
        const auto low = address & ~(page_ - 1), high = (address + size + page_ - 1) & ~(page_ - 1);
        protect(low, high - low, PROT_READ | PROT_WRITE);
        alias_bytes_ += size - allocated;
    }
    return 0;
}
uint32_t GuestMemory::set_permission(uint64_t address, uint64_t size, uint32_t permission) {
    std::lock_guard lock(mutex_);
    if (address % guest_page)
        return error(102);
    if (!size || size % guest_page)
        return error(101);
    if (permission != 0 && permission != 1 && permission != 3)
        return error(108);
    if (!contains(base_, size_, address, size))
        return error(110);
    for (auto at = address; at < address + size;) {
        auto region = query_locked(at);
        if (region.state != 5 || region.attribute)
            return error(106);
        at = region.base + region.size;
    }
    replace(address, size, 5, permission);
    protect_logical(address, size);
    return 0;
}
uint32_t GuestMemory::map_stack(uint64_t destination, uint64_t source, uint64_t size, bool unmap) {
    std::lock_guard lock(mutex_);
    if ((destination | source) % guest_page)
        return error(102);
    if (!size || size % guest_page)
        return error(101);
    if (!contains(stack(), config_.stack_size, destination, size) ||
        !(contains(heap(), heap_size_, source, size) ||
          contains(alias(), config_.alias_size, source, size)))
        return error(110);
    if (unmap) {
        auto mapping = stack_mappings_.find(destination);
        if (mapping == stack_mappings_.end() || mapping->second.source != source ||
            mapping->second.size != size)
            return error(106);
        replace(source, size, 5, 3);
        protect_logical(source, size);
        memcpy(reinterpret_cast<void *>(source), reinterpret_cast<void *>(destination), size);
        replace(destination, size, 0, 0);
        discard(destination, size);
        stack_mappings_.erase(mapping);
    } else {
        for (auto at = source; at < source + size;) {
            auto region = query_locked(at);
            if (region.state != 5 || region.permission != 3 || region.attribute)
                return error(106);
            at = region.base + region.size;
        }
        for (auto at = destination; at < destination + size;) {
            auto region = query_locked(at);
            if (region.state != 0)
                return error(106);
            at = region.base + region.size;
        }
        // Source pages are locked and inaccessible while mapped as stack. A
        // private copy is sufficient because no legal source accesses remain.
        stack_mappings_.emplace(destination, StackMapping{source, size});
        replace(destination, size, 0xb, 3);
        protect_logical(destination, size);
        memcpy(reinterpret_cast<void *>(destination), reinterpret_cast<void *>(source), size);
        replace(source, size, 5, 0, 1);
        protect_logical(source, size);
    }
    return 0;
}
} // namespace mng::port
