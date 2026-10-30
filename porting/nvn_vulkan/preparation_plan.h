// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace mng::vulkan::preparation {
inline void bump(uint64_t &revision) {
    if (!++revision) std::abort();
}
inline bool assign_bytes(void *target, const void *source, size_t bytes,
                         std::initializer_list<uint64_t *> revisions) {
    if (!bytes || !std::memcmp(target, source, bytes)) return false;
    std::memcpy(target, source, bytes);
    for (auto *revision : revisions) bump(*revision);
    return true;
}
template<class T>
bool assign(T &target, const T &source, std::initializer_list<uint64_t *> revisions) {
    return assign_bytes(&target, &source, sizeof(T), revisions);
}
struct BufferRevision { uint64_t address{1}, shape{1}; };
template<class Range>
void assign_range(Range &target, const Range &source, BufferRevision &revision) {
    if (target.address != source.address) bump(revision.address);
    // A dynamic UBO's key contains its length, not its nonzero address. Unbound
    // ranges use the legacy address/size key and therefore still track both.
    if (target.size != source.size || bool(target.address) != bool(source.address) ||
        (!source.size && target.address != source.address)) bump(revision.shape);
    target = source;
}
struct Versions {
    uint64_t fixed{1}, vertex{1}, blend{1}, support{1}, pools{1};
    BufferRevision uniform[6][16], storage[6][16];
    std::array<uint64_t, 6> textures{}, samplers{}, images{};
};
// Watches refer only to stable host-side revision counters, never Guest data.
struct RevisionWatch {
    std::vector<const uint64_t *> sources;
    std::vector<uint64_t> values;
    bool valid{};
    void add(const uint64_t &source) {
        if (std::find(sources.begin(), sources.end(), &source) == sources.end())
            sources.push_back(&source);
    }
    bool unchanged() const {
        if (!valid || sources.size() != values.size()) return false;
        for (size_t i = 0; i < sources.size(); ++i)
            if (*sources[i] != values[i]) return false;
        return true;
    }
    void remember() {
        values.clear();
        for (auto *source : sources) values.push_back(*source);
        valid = true;
    }
};
template<class Binding>
struct ShaderPlan {
    struct Group { Binding binding; bool written; };
    struct Element { uint32_t group, element; };
    bool ready{}, valid{};
    unsigned uniforms{}, varying_uniforms{};
    std::vector<Group> groups;
    std::vector<Element> elements;
};
// Metadata is copied by value. No plan points into a Shader's resizable vectors.
// Large/unrecognized inputs fall back to the renderer, not a truncated plan.
template<class Binding, class Written, class Uniform, class Varying>
bool build(ShaderPlan<Binding> &plan, const std::vector<Binding> &bindings,
           Written written, Uniform uniform, Varying varying) {
    plan = {};
    plan.ready = true;
    size_t elements = 0;
    for (const auto &binding : bindings) {
        if (!binding.count || binding.count > 128 || elements + binding.count > 4096)
            return false;
        elements += binding.count;
    }
    plan.groups.reserve(bindings.size());
    plan.elements.reserve(elements);
    for (const auto &binding : bindings) {
        const auto group = uint32_t(plan.groups.size());
        plan.groups.push_back({binding, written(binding)});
        if (uniform(binding)) {
            plan.uniforms += binding.count;
            if (varying(binding)) plan.varying_uniforms += binding.count;
        }
        for (uint32_t i = 0; i < binding.count; ++i)
            plan.elements.push_back({group, i});
    }
    plan.valid = true;
    return true;
}
struct StorageRange { uint64_t begin, end; bool written; };
inline void merge_ranges(std::vector<StorageRange> &ranges) {
    std::sort(ranges.begin(), ranges.end(), [](auto a, auto b) { return a.begin < b.begin; });
    size_t count = 0;
    for (auto range : ranges) {
        if (count && range.begin < ranges[count - 1].end) {
            auto &previous = ranges[count - 1];
            previous.end = std::max(previous.end, range.end);
            previous.written |= range.written;
        } else ranges[count++] = range;
    }
    ranges.resize(count);
}
}
