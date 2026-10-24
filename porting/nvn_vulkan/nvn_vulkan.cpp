// SPDX-License-Identifier: MIT
// Native NVN frontend. Command layouts and texture storage are supplied by the
// pinned MIT D2R renderer; no title-specific host or patch code is linked.
#include "nvn_vulkan.h"
#include "engine/xbg_game_internal.h"
#include "nvn_protos.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
std::recursive_mutex front_mutex;
bool ready;
std::map<std::string, void *> &functions() {
    static std::map<std::string, void *> table;
    return table;
}
struct Register {
    Register(const char *name, void *address) { functions().emplace(name, address); }
};
#define API(name, result, ...)                                                                     \
    static result name(__VA_ARGS__);                                                               \
    [[maybe_unused]] static const NVNT_##name signature_##name = name;                             \
    static Register register_##name(#name, reinterpret_cast<void *>(name));                        \
    static result name(__VA_ARGS__)
#define LOCK std::lock_guard guard(front_mutex)
void need(bool condition, const char *message) {
    if (!condition)
        xbg_fatalf("Native NVN: %s", message);
}
// Store only an eight-byte host handle in opaque NVN objects. The native
// Zelda ABI reserves less space than D2R for several objects (notably texture);
// engine structs must never be written directly into guest storage.
struct Object {
    virtual ~Object() = default;
};
template <class T> struct Box final : Object {
    T value{};
};
std::map<uint64_t, std::unique_ptr<Object>> objects;
uint64_t next_object = 0x4d4e470000000000ull;
xbg_game_config host_config{};
template <class T, class U> T &as(U *p) {
    std::lock_guard guard(front_mutex);
    static_assert(sizeof(U) >= 8);
    need(p != nullptr, "Null object");
    uint64_t id = 0;
    memcpy(&id, p, 8);
    auto it = objects.find(id);
    if (it != objects.end())
        if (auto *box = dynamic_cast<Box<T> *>(it->second.get()))
            return box->value;
    need(objects.size() < 1048576, "Native object retention limit");
    auto box = std::make_unique<Box<T>>();
    auto *value = &box->value;
    id = ++next_object;
    objects.emplace(id, std::move(box));
    memcpy(p, &id, 8);
    return *value;
}
template <class T, class U> const T &as(const U *p) {
    std::lock_guard guard(front_mutex);
    static_assert(sizeof(U) >= 8);
    need(p != nullptr, "Null object");
    uint64_t id = 0;
    memcpy(&id, p, 8);
    auto it = objects.find(id);
    need(it != objects.end(), "Uninitialized object");
    auto *box = dynamic_cast<Box<T> *>(it->second.get());
    need(box != nullptr, "Wrong object kind");
    return box->value;
}
std::map<cmd_t *, NVNcommandBuffer *> command_owners;
bool range(uint64_t size, int64_t offset, uint64_t count) {
    return offset >= 0 && uint64_t(offset) <= size && count <= size - uint64_t(offset);
}
std::map<uint64_t, std::unique_ptr<tex_t>> texture_snapshots;
const tex_t *snapshot(const NVNtexture *texture) {
    auto &t = as<tex_t>(texture);
    need(t.magic == MAGIC('T', 'E', 'X', 0), "Invalid texture");
    auto &copy = texture_snapshots[t.serial];
    if (!copy) {
        need(texture_snapshots.size() <= 65536, "Texture snapshot limit");
        copy = std::make_unique<tex_t>(t);
    }
    return copy.get();
}
// By-value texture views fit the 16-byte storage used by the native Zelda
// frontend. This is our representation; convert to the engine's wider view.
struct CompactView {
    uint16_t level, levels, layer, layers, format;
    uint8_t target, flags;
    uint32_t reserved;
};
static_assert(sizeof(CompactView) == 16);
CompactView read_view(const NVNtextureView *p) {
    CompactView v{};
    if (p)
        memcpy(&v, p, sizeof(v));
    return v;
}
void write_view(NVNtextureView *p, const CompactView &v) {
    need(p, "Null view");
    memcpy(p, &v, sizeof(v));
}
view_t translate_view(const NVNtextureView *p) {
    auto v = read_view(p);
    return {uint8_t(v.flags & 1),
            uint8_t((v.flags >> 1) & 1),
            uint8_t((v.flags >> 2) & 1),
            uint8_t((v.flags >> 3) & 1),
            v.level,
            v.levels,
            v.layer,
            v.layers,
            v.format,
            v.target};
}
API(nvnTextureViewSetDefaults, void, NVNtextureView *v) { write_view(v, {}); }
API(nvnTextureViewSetLevels, void, NVNtextureView *v, int first, int count) {
    need(first >= 0 && first <= 65535 && count > 0 && count <= 65535, "View mip range");
    auto s = read_view(v);
    s.level = first;
    s.levels = count;
    s.flags |= 1;
    write_view(v, s);
}
API(nvnTextureViewSetLayers, void, NVNtextureView *v, int first, int count) {
    need(first >= 0 && first <= 65535 && count > 0 && count <= 65535, "View layer range");
    auto s = read_view(v);
    s.layer = first;
    s.layers = count;
    s.flags |= 2;
    write_view(v, s);
}
API(nvnTextureViewSetFormat, void, NVNtextureView *v, NVNformat f) {
    need(int(f) >= 0 && int(f) <= 65535, "View format");
    auto s = read_view(v);
    s.format = f;
    s.flags |= 4;
    write_view(v, s);
}
API(nvnTextureViewSetTarget, void, NVNtextureView *v, NVNtextureTarget t) {
    need(int(t) >= 0 && int(t) <= 255, "View target");
    auto s = read_view(v);
    s.target = t;
    s.flags |= 8;
    write_view(v, s);
}
API(nvnTextureViewGetLevels, NVNboolean, const NVNtextureView *v, int *first, int *count) {
    auto s = read_view(v);
    if (!(s.flags & 1))
        return 0;
    *first = s.level;
    *count = s.levels;
    return 1;
}
API(nvnTextureViewGetLayers, NVNboolean, const NVNtextureView *v, int *first, int *count) {
    auto s = read_view(v);
    if (!(s.flags & 2))
        return 0;
    *first = s.layer;
    *count = s.layers;
    return 1;
}
API(nvnTextureViewGetFormat, NVNboolean, const NVNtextureView *v, NVNformat *f) {
    auto s = read_view(v);
    if (!(s.flags & 4))
        return 0;
    *f = NVNformat(s.format);
    return 1;
}
API(nvnTextureViewGetTarget, NVNboolean, const NVNtextureView *v, NVNtextureTarget *t) {
    auto s = read_view(v);
    if (!(s.flags & 8))
        return 0;
    *t = NVNtextureTarget(s.target);
    return 1;
}
void segment(cmd_t &c, uint8_t *p, size_t bytes) {
    need(p && bytes >= 32, "Invalid command segment");
    if (c.recording && c.cur) {
        need(c.end - c.cur >= 16, "No command chain reserve");
        op_hdr jump{OP_JUMP, 16};
        memcpy(c.cur, &jump, 8);
        uint64_t target = uint64_t(p);
        memcpy(c.cur + 8, &target, 8);
    }
    c.cur = p;
    c.end = p + bytes;
}
uint8_t *allocate(cmd_t &c, size_t bytes) {
    need(c.recording && bytes <= 64u * 1024 * 1024, "Invalid command recording");
    bytes = (bytes + 7) & ~size_t(7);
    for (unsigned attempt = 0; !c.cur || size_t(c.end - c.cur) < bytes + 16; ++attempt) {
        need(attempt < 2, "Command allocator did not replenish memory");
        if (c.cb) {
            const uint64_t args[] = {uint64_t(command_owners.at(&c)),
                                     NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY,
                                     bytes + 16, uint64_t(c.cb_data)};
            if (host_config.invoke_guest)
                host_config.invoke_guest(host_config.userdata, reinterpret_cast<void *>(c.cb), args,
                                         4);
            else
                c.cb(command_owners.at(&c), NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY,
                     bytes + 16, c.cb_data);
        } else {
            auto size = std::max<size_t>(65536, bytes + 16);
            segment(c, static_cast<uint8_t *>(xbg_host_command_memory(&c, size)), size);
        }
    }
    auto *out = c.cur;
    c.cur += bytes;
    memset(out, 0, bytes);
    return out;
}
template <class T> void record(NVNcommandBuffer *c, uint32_t op, const T &value) {
    auto bytes = (8 + sizeof(T) + 7) & ~size_t(7);
    auto *out = allocate(as<cmd_t>(c), bytes);
    op_hdr h{op, uint32_t(bytes)};
    memcpy(out, &h, 8);
    memcpy(out + 8, &value, sizeof(T));
}
API(nvnDeviceBuilderSetDefaults, void, NVNdeviceBuilder *b) { as<dev_builder_t>(b) = {}; }
API(nvnDeviceBuilderSetFlags, void, NVNdeviceBuilder *b, int flags) {
    as<dev_builder_t>(b).flags = flags;
}
API(nvnDeviceInitialize, NVNboolean, NVNdevice *d, const NVNdeviceBuilder *b) {
    LOCK;
    need(ready, "Vulkan is not initialized");
    as<dev_t_>(d) = {MAGIC('D', 'E', 'V', 0), as<dev_builder_t>(b).flags};
    return 1;
}
API(nvnDeviceFinalize, void, NVNdevice *d) {
    LOCK;
    xbg_legacy_finish();
    as<dev_t_>(d).magic = 0;
}
API(nvnDeviceGetProcAddress, PFNNVNGENERICFUNCPTRPROC, const NVNdevice *, const char *name) {
    return reinterpret_cast<PFNNVNGENERICFUNCPTRPROC>(mng_nvn_get_proc_address(name));
}
API(nvnDeviceGetInteger, void, const NVNdevice *, NVNdeviceInfo property, int *out) {
    LOCK;
    need(xbg_device_integer(property, out) != 0, "Unsupported device property");
}
API(nvnDeviceFinalizeCommandHandle, void, NVNdevice *, NVNcommandHandle handle) {
    LOCK;
    xbg_release_command_handle(handle);
}

API(nvnMemoryPoolBuilderSetDefaults, void, NVNmemoryPoolBuilder *b) {
    auto *d = as<pool_builder_t>(b).dev;
    as<pool_builder_t>(b) = {};
    as<pool_builder_t>(b).dev = d;
}
API(nvnMemoryPoolBuilderSetDevice, void, NVNmemoryPoolBuilder *b, NVNdevice *d) {
    as<pool_builder_t>(b).dev = d;
}
API(nvnMemoryPoolBuilderSetStorage, void, NVNmemoryPoolBuilder *b, void *p, size_t n) {
    as<pool_builder_t>(b).mem = p;
    as<pool_builder_t>(b).size = n;
}
API(nvnMemoryPoolBuilderSetFlags, void, NVNmemoryPoolBuilder *b, int f) {
    as<pool_builder_t>(b).flags = f;
}
API(nvnMemoryPoolInitialize, NVNboolean, NVNmemoryPool *p, const NVNmemoryPoolBuilder *b) {
    LOCK;
    auto &s = as<pool_builder_t>(b);
    if (!s.mem || !s.size ||
        (s.flags & (NVN_MEMORY_POOL_FLAGS_VIRTUAL | NVN_MEMORY_POOL_FLAGS_PHYSICAL)))
        return 0;
    if (s.size > UINTPTR_MAX - uintptr_t(s.mem))
        return 0;
    as<pool_t>(p) = {MAGIC('P', 'O', 'O', 'L'), s.flags, static_cast<uint8_t *>(s.mem), s.size};
    xbg_legacy_pool_add(s.mem, s.size);
    return 1;
}
API(nvnMemoryPoolFinalize, void, NVNmemoryPool *p) {
    LOCK;
    auto &s = as<pool_t>(p);
    xbg_legacy_finish();
    xbg_legacy_pool_del(s.mem);
    s = {};
}
API(nvnMemoryPoolMap, void *, const NVNmemoryPool *p) { return as<pool_t>(p).mem; }
API(nvnMemoryPoolGetBufferAddress, NVNbufferAddress, const NVNmemoryPool *p) {
    return uintptr_t(as<pool_t>(p).mem);
}
API(nvnMemoryPoolGetSize, size_t, const NVNmemoryPool *p) { return as<pool_t>(p).size; }
API(nvnMemoryPoolGetFlags, NVNmemoryPoolFlags, const NVNmemoryPool *p) {
    return NVNmemoryPoolFlags(as<pool_t>(p).flags);
}
API(nvnMemoryPoolFlushMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t bytes) {
    LOCK;
    need(range(as<pool_t>(p).size, off, bytes), "Pool flush range");
    std::atomic_thread_fence(std::memory_order_release);
}
API(nvnMemoryPoolInvalidateMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t bytes) {
    LOCK;
    need(range(as<pool_t>(p).size, off, bytes), "Pool invalidate range");
    xbg_legacy_finish();
    std::atomic_thread_fence(std::memory_order_acquire);
}
struct Buffer {
    const NVNmemoryPool *pool;
    ptrdiff_t offset;
    size_t bytes;
};
API(nvnBufferBuilderSetDefaults, void, NVNbufferBuilder *b) { as<Buffer>(b) = {}; }
API(nvnBufferBuilderSetDevice, void, NVNbufferBuilder *, NVNdevice *) {} // Device is process-owned.
API(nvnBufferBuilderSetStorage, void, NVNbufferBuilder *b, NVNmemoryPool *p, ptrdiff_t o,
    size_t n) {
    as<Buffer>(b) = {p, o, n};
}
API(nvnBufferInitialize, NVNboolean, NVNbuffer *b, const NVNbufferBuilder *builder) {
    auto s = as<Buffer>(builder);
    if (!s.pool || !range(as<pool_t>(s.pool).size, s.offset, s.bytes))
        return 0;
    as<Buffer>(b) = s;
    return 1;
}
API(nvnBufferFinalize, void, NVNbuffer *b) { as<Buffer>(b) = {}; }
API(nvnBufferMap, void *, const NVNbuffer *b) {
    auto &s = as<Buffer>(b);
    return as<pool_t>(s.pool).mem + s.offset;
}
API(nvnBufferGetAddress, NVNbufferAddress, const NVNbuffer *b) {
    return uintptr_t(nvnBufferMap(b));
}
API(nvnBufferGetSize, size_t, const NVNbuffer *b) { return as<Buffer>(b).bytes; }
API(nvnBufferFlushMappedRange, void, const NVNbuffer *b, ptrdiff_t o, size_t n) {
    auto &s = as<Buffer>(b);
    need(range(s.bytes, o, n), "Buffer flush range");
    nvnMemoryPoolFlushMappedRange(s.pool, s.offset + o, n);
}
API(nvnBufferInvalidateMappedRange, void, const NVNbuffer *b, ptrdiff_t o, size_t n) {
    auto &s = as<Buffer>(b);
    need(range(s.bytes, o, n), "Buffer invalidate range");
    nvnMemoryPoolInvalidateMappedRange(s.pool, s.offset + o, n);
}

API(nvnTextureBuilderSetDefaults, void, NVNtextureBuilder *b) {
    auto *d = as<tex_builder_t>(b).dev;
    auto &t = as<tex_builder_t>(b);
    t = {};
    t.dev = d;
    t.d.target = NVN_TEXTURE_TARGET_2D;
    t.d.width = t.d.height = t.d.depth = t.d.levels = 1;
}
API(nvnTextureBuilderSetDevice, void, NVNtextureBuilder *b, NVNdevice *d) {
    as<tex_builder_t>(b).dev = d;
}
#define TEX_SET(Name, Field, Type)                                                                 \
    API(nvnTextureBuilderSet##Name, void, NVNtextureBuilder *b, Type v) {                          \
        as<tex_builder_t>(b).d.Field = v;                                                          \
    }
TEX_SET(Flags, flags, int)
TEX_SET(Target, target, NVNtextureTarget)
TEX_SET(Format, format, NVNformat) TEX_SET(Width, width, int) TEX_SET(Height, height, int)
    TEX_SET(Depth, depth, int) TEX_SET(Levels, levels, int) TEX_SET(Samples, samples, int)
        TEX_SET(Stride, stride, ptrdiff_t)
            API(nvnTextureBuilderSetSize2D, void, NVNtextureBuilder *b, int w, int h) {
    auto &d = as<tex_builder_t>(b).d;
    d.width = w;
    d.height = h;
    d.depth = 1;
}
API(nvnTextureBuilderSetSize3D, void, NVNtextureBuilder *b, int w, int h, int z) {
    auto &d = as<tex_builder_t>(b).d;
    d.width = w;
    d.height = h;
    d.depth = z;
}
API(nvnTextureBuilderSetStorage, void, NVNtextureBuilder *b, NVNmemoryPool *p, ptrdiff_t o) {
    as<tex_builder_t>(b).pool = &as<pool_t>(p);
    as<tex_builder_t>(b).off = o;
}
API(nvnTextureBuilderSetPackagedTextureLayout, void, NVNtextureBuilder *b,
    const NVNpackagedTextureLayout *layout) {
    auto &t = as<tex_builder_t>(b);
    t.has_layout = layout != nullptr;
    if (layout)
        memcpy(t.layout, layout, 8);
    t.d.pkg_block_h = layout ? t.layout[0] + 1 : 0;
}
API(nvnTextureBuilderGetStorageSize, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout l{};
    nvn_tex_layout(&as<tex_builder_t>(b).d, &l);
    return l.size;
}
API(nvnTextureBuilderGetStorageAlignment, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout l{};
    nvn_tex_layout(&as<tex_builder_t>(b).d, &l);
    return l.align;
}
API(nvnTextureInitialize, NVNboolean, NVNtexture *t, const NVNtextureBuilder *b) {
    LOCK;
    auto &s = as<tex_builder_t>(b);
    nvn_texlayout l{};
    nvn_tex_layout(&s.d, &l);
    if (!l.bpb || !l.size || !s.pool || !range(s.pool->size, s.off, l.size))
        return 0;
    auto &o = as<tex_t>(t);
    o = {};
    o.magic = MAGIC('T', 'E', 'X', 0);
    o.d = s.d;
    o.L = l;
    o.pool = s.pool;
    o.off = s.off;
    o.mem = s.pool->mem + s.off;
    o.serial = nvn_new_serial();
    o.has_layout = s.has_layout;
    memcpy(o.layout, s.layout, 8);
    return 1;
}
API(nvnTextureFinalize, void, NVNtexture *t) {
    LOCK;
    xbg_legacy_finish();
    xbg_legacy_tex_release(&as<tex_t>(t));
    as<tex_t>(t).magic = 0;
}
#define TEX_GET(Name, Field, Type)                                                                 \
    API(nvnTextureGet##Name, Type, const NVNtexture *t) { return Type(as<tex_t>(t).d.Field); }
TEX_GET(Width, width, int)
TEX_GET(Height, height, int)
TEX_GET(Depth, depth, int) TEX_GET(Levels, levels, int) TEX_GET(Format, format, NVNformat)
    TEX_GET(Target, target, NVNtextureTarget)
        API(nvnTextureReadTexels, void, const NVNtexture *t, const NVNtextureView *v,
            const NVNcopyRegion *r, void *out) {
    LOCK;
    auto view = translate_view(v);
    need(!(view.set_format || view.set_target),
         "Readback reinterpretation requires a staging view");
    need(r && out, "Null readback");
    xbg_legacy_read_texels(&as<tex_t>(t), view.set_levels ? view.base_level : 0,
                           view.set_layers ? view.min_layer : 0, r, static_cast<uint8_t *>(out), 0,
                           0);
}

API(nvnCommandBufferInitialize, NVNboolean, NVNcommandBuffer *c, NVNdevice *d) {
    as<cmd_t>(c) = {};
    as<cmd_t>(c).magic = MAGIC('C', 'M', 'D', 0);
    as<cmd_t>(c).dev = d;
    command_owners[&as<cmd_t>(c)] = c;
    return 1;
}
API(nvnCommandBufferFinalize, void, NVNcommandBuffer *c) {
    LOCK;
    need(!as<cmd_t>(c).recording, "Finalize while recording");
    as<cmd_t>(c) = {};
}
API(nvnCommandBufferSetMemoryCallback, void, NVNcommandBuffer *c,
    PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb) {
    as<cmd_t>(c).cb = cb;
}
API(nvnCommandBufferSetMemoryCallbackData, void, NVNcommandBuffer *c, void *data) {
    as<cmd_t>(c).cb_data = data;
}
API(nvnCommandBufferAddCommandMemory, void, NVNcommandBuffer *c, const NVNmemoryPool *p,
    ptrdiff_t o, size_t n) {
    LOCK;
    auto &pool = as<pool_t>(p);
    need(range(pool.size, o, n), "Command memory range");
    segment(as<cmd_t>(c), pool.mem + o, n);
}
API(nvnCommandBufferAddControlMemory, void, NVNcommandBuffer *, void *, size_t) {
} // Control records live in the bounded host arena.
API(nvnCommandBufferBeginRecording, void, NVNcommandBuffer *c) {
    LOCK;
    auto &s = as<cmd_t>(c);
    need(!s.recording, "Nested recording");
    s.recording = 1;
    auto *out = allocate(s, 8);
    op_hdr h{OP_BEGIN, 8};
    memcpy(out, &h, 8);
    s.begin = out;
}
API(nvnCommandBufferEndRecording, NVNcommandHandle, NVNcommandBuffer *c) {
    LOCK;
    auto &s = as<cmd_t>(c);
    auto *out = allocate(s, 8);
    op_hdr h{OP_END, 8};
    memcpy(out, &h, 8);
    s.recording = 0;
    return uintptr_t(s.begin);
}
API(nvnCommandBufferIsRecording, NVNboolean, const NVNcommandBuffer *c) {
    return as<cmd_t>(c).recording != 0;
}
API(nvnCommandBufferCopyCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *h) {
    LOCK;
    need(n >= 0 && (!n || h), "Command copy arguments");
    xbg_copy_commands(&as<cmd_t>(c), n, h);
}
API(nvnCommandBufferCallCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *h) {
    LOCK;
    need(n >= 0 && (!n || h), "Command call arguments");
    for (int i = 0; i < n; ++i)
        record(c, OP_CALL, h[i]);
}
API(nvnCommandBufferCopyBufferToBuffer, void, NVNcommandBuffer *c, NVNbufferAddress src,
    NVNbufferAddress dst, size_t size, int flags) {
    LOCK;
    need(!flags, "Unsupported buffer copy flags");
    const uint64_t args[]{src, dst, size};
    record(c, OP_COPY_B2B, args);
}
API(nvnCommandBufferUpdateUniformBuffer, void, NVNcommandBuffer *c, NVNbufferAddress buffer,
    size_t capacity, ptrdiff_t offset, size_t bytes, const void *data) {
    LOCK;
    need(range(capacity, offset, bytes) && data && bytes <= 65536, "Uniform update range");
    auto total = (24 + bytes + 7) & ~size_t(7);
    auto *out = allocate(as<cmd_t>(c), total);
    op_hdr h{OP_UPDATE_UB, uint32_t(total)};
    uint64_t args[]{buffer + offset, bytes};
    memcpy(out, &h, 8);
    memcpy(out + 8, args, 16);
    memcpy(out + 24, data, bytes);
}
API(nvnCommandBufferSetRenderTargets, void, NVNcommandBuffer *c, int n,
    const NVNtexture *const *color, const NVNtextureView *const *views, const NVNtexture *depth,
    const NVNtextureView *depth_view) {
    LOCK;
    need(n >= 0 && n <= 8 && (!n || color), "Render target count");
    p_set_rt p{};
    p.n = n;
    for (int i = 0; i < n; ++i) {
        if (color[i])
            p.color[i] = snapshot(color[i]);
        if (views && views[i]) {
            p.has_cview[i] = 1;
            p.cview[i] = translate_view(views[i]);
        }
    }
    if (depth)
        p.depth = snapshot(depth);
    if (depth_view) {
        p.has_dview = 1;
        p.dview = translate_view(depth_view);
    }
    record(c, OP_SET_RT, p);
}
API(nvnCommandBufferClearColor, void, NVNcommandBuffer *c, int index, const float *rgba, int mask) {
    LOCK;
    need(rgba && index >= 0 && index < 8 && !(mask & ~15), "Clear color arguments");
    p_clear_color p{index, mask, {}};
    memcpy(p.rgba, rgba, 16);
    record(c, OP_CLEAR_COLOR, p);
}
API(nvnCommandBufferClearDepthStencil, void, NVNcommandBuffer *c, float depth, uint32_t write,
    int stencil, int mask) {
    LOCK;
    record(c, OP_CLEAR_DS, p_clear_ds{depth, int(write & 255), stencil, mask});
}
API(nvnCommandBufferSetScissor, void, NVNcommandBuffer *c, int x, int y, int w, int h) {
    LOCK;
    record(c, OP_SCISSOR, p_scissor{x, y, w, h});
}
API(nvnCommandBufferSetDepthRange, void, NVNcommandBuffer *c, float n, float f) {
    LOCK;
    record(c, OP_DEPTH_RANGE, p_depth_range{n, f});
}
API(nvnCommandBufferSetViewport, void, NVNcommandBuffer *c, int x, int y, int w, int h) {
    LOCK;
    p_viewports p{};
    p.n = 1;
    p.v[0][0] = x;
    p.v[0][1] = y;
    p.v[0][2] = w;
    p.v[0][3] = h;
    p.v[0][5] = 1;
    record(c, OP_VIEWPORTS, p);
}
API(nvnCommandBufferBarrier, void, NVNcommandBuffer *c, int bits) {
    LOCK;
    record(c, OP_BARRIER, uint32_t(bits));
}

// Program metadata is copied at bind time; shader control/code remain in the
// caller's immutable shader pool. The engine loads matching offline XGS/SPIR-V.
std::map<uint64_t, std::unique_ptr<prog_t>> program_snapshots;
API(nvnProgramInitialize, NVNboolean, NVNprogram *p, NVNdevice *) {
    LOCK;
    as<prog_t>(p) = {};
    as<prog_t>(p).magic = MAGIC('P', 'R', 'G', 0);
    as<prog_t>(p).serial = nvn_new_serial();
    return 1;
}
API(nvnProgramSetShaders, NVNboolean, NVNprogram *p, int n, const NVNshaderData *shaders) {
    LOCK;
    if (n < 1 || n > 6 || !shaders)
        return 0;
    auto &program = as<prog_t>(p);
    program.nstage = n;
    memcpy(program.sd, shaders, n * sizeof(*shaders));
    program.serial = nvn_new_serial();
    return 1;
}
API(nvnProgramFinalize, void, NVNprogram *p) {
    LOCK;
    xbg_legacy_finish();
    xbg_legacy_prog_release(&as<prog_t>(p));
    as<prog_t>(p) = {};
}
API(nvnCommandBufferBindProgram, void, NVNcommandBuffer *c, const NVNprogram *p, int stages) {
    LOCK;
    p_bind_program payload{};
    payload.stages = stages;
    if (p) {
        auto &program = as<prog_t>(p);
        need(program.magic == MAGIC('P', 'R', 'G', 0), "Invalid program");
        auto &snapshot = program_snapshots[program.serial];
        if (!snapshot) {
            need(program_snapshots.size() <= 65536, "Program snapshot limit");
            snapshot = std::make_unique<prog_t>(program);
        }
        payload.prog = snapshot.get();
    }
    record(c, OP_BIND_PROGRAM, payload);
}
API(nvnCommandBufferBindUniformBuffer, void, NVNcommandBuffer *c, NVNshaderStage stage, int index,
    NVNbufferAddress address, size_t bytes) {
    LOCK;
    record(c, OP_BIND_UBO, p_bind_buf{stage, index, address, bytes});
}
API(nvnCommandBufferBindStorageBuffer, void, NVNcommandBuffer *c, NVNshaderStage stage, int index,
    NVNbufferAddress address, size_t bytes) {
    LOCK;
    record(c, OP_BIND_SSBO, p_bind_buf{stage, index, address, bytes});
}
API(nvnCommandBufferBindVertexBuffer, void, NVNcommandBuffer *c, int index,
    NVNbufferAddress address, size_t bytes) {
    LOCK;
    need(index >= 0 && index < 16, "Vertex binding");
    p_bind_vbuf p{};
    p.first = index;
    p.n = 1;
    p.r[0] = {address, bytes};
    record(c, OP_BIND_VBUF, p);
}
API(nvnCommandBufferBindVertexBuffers, void, NVNcommandBuffer *c, int first, int n,
    const NVNbufferRange *ranges) {
    LOCK;
    need(first >= 0 && n >= 0 && n <= 16 - first && (!n || ranges), "Vertex binding range");
    p_bind_vbuf p{};
    p.first = first;
    p.n = n;
    if (n)
        memcpy(p.r, ranges, n * sizeof(*ranges));
    record(c, OP_BIND_VBUF, p);
}
API(nvnCommandBufferDrawArrays, void, NVNcommandBuffer *c, NVNdrawPrimitive primitive, int first,
    int count) {
    LOCK;
    need(first >= 0 && count >= 0, "Draw range");
    record(c, OP_DRAW, p_draw{primitive, first, count});
}
API(nvnCommandBufferDrawElementsInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive primitive,
    NVNindexType type, int count, NVNbufferAddress indices, int base_vertex, int base_instance,
    int instances) {
    LOCK;
    need(count >= 0 && base_instance >= 0 && instances >= 0, "Indexed draw range");
    record(c, OP_DRAW_IDX,
           p_draw_idx{primitive, type, count, base_vertex, base_instance, instances, indices});
}
API(nvnCommandBufferDrawElements, void, NVNcommandBuffer *c, NVNdrawPrimitive primitive,
    NVNindexType type, int count, NVNbufferAddress indices) {
    nvnCommandBufferDrawElementsInstanced(c, primitive, type, count, indices, 0, 0, 1);
}
API(nvnCommandBufferDispatchCompute, void, NVNcommandBuffer *c, int x, int y, int z) {
    LOCK;
    need(x >= 0 && y >= 0 && z >= 0, "Dispatch range");
    record(c, OP_DISPATCH, p_dispatch{x, y, z, 0});
}
API(nvnTexturePoolInitialize, NVNboolean, NVNtexturePool *p, const NVNmemoryPool *pool,
    ptrdiff_t offset, int count) {
    LOCK;
    if (count < 0 || !pool || !range(as<pool_t>(pool).size, offset, uint64_t(count) * 32))
        return 0;
    as<dpool_t>(p) = {&as<pool_t>(pool), offset, count};
    return 1;
}
API(nvnTexturePoolRegisterTexture, void, const NVNtexturePool *p, int id, const NVNtexture *texture,
    const NVNtextureView *view) {
    LOCK;
    auto v = translate_view(view);
    xbg_register_texture(&as<dpool_t>(p), id, snapshot(texture), view ? &v : nullptr);
}
API(nvnCommandBufferSetTexturePool, void, NVNcommandBuffer *c, const NVNtexturePool *p) {
    LOCK;
    record(c, OP_SET_TEX_POOL, p_set_pool{&as<dpool_t>(p)});
}
API(nvnCommandBufferBindTexture, void, NVNcommandBuffer *c, NVNshaderStage stage, int index,
    NVNtextureHandle handle) {
    LOCK;
    p_bind_handles p{};
    p.stage = stage;
    p.first = index;
    p.n = 1;
    p.h[0] = handle;
    record(c, OP_BIND_TEX, p);
}

API(nvnQueueBuilderSetDefaults, void, NVNqueueBuilder *b) {
    auto *d = as<queue_builder_t>(b).dev;
    as<queue_builder_t>(b) = {};
    as<queue_builder_t>(b).dev = d;
}
API(nvnQueueBuilderSetDevice, void, NVNqueueBuilder *b, NVNdevice *d) {
    as<queue_builder_t>(b).dev = d;
}
API(nvnQueueBuilderSetFlags, void, NVNqueueBuilder *b, int f) { as<queue_builder_t>(b).flags = f; }
API(nvnQueueInitialize, NVNboolean, NVNqueue *q, const NVNqueueBuilder *b) {
    as<queue_t>(q) = {MAGIC('Q', 'U', 'E', 0), as<queue_builder_t>(b).dev, 0};
    return 1;
}
API(nvnQueueSubmitCommands, void, NVNqueue *q, int n, const NVNcommandHandle *handles) {
    LOCK;
    need(n >= 0 && (!n || handles), "Submission arguments");
    xbg_submit(n, handles);
    ++as<queue_t>(q).submits;
}
API(nvnQueueFlush, void, NVNqueue *) {
    LOCK;
    xbg_legacy_finish();
} // Conservative: flush submits and drains; optimize after correctness.
API(nvnQueueFinish, void, NVNqueue *) {
    LOCK;
    xbg_legacy_finish();
}
API(nvnQueueFinalize, void, NVNqueue *q) {
    LOCK;
    xbg_legacy_finish();
    as<queue_t>(q) = {};
}
API(nvnSyncInitialize, NVNboolean, NVNsync *s, NVNdevice *) {
    as<sync_t>(s) = {MAGIC('S', 'Y', 'N', 'C'), 0, 0};
    return 1;
}
API(nvnSyncFinalize, void, NVNsync *s) { as<sync_t>(s) = {}; }
API(nvnQueueFenceSync, void, NVNqueue *, NVNsync *s, NVNsyncCondition condition, int flags) {
    LOCK;
    need(condition == NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE && !(flags & ~1u),
         "Unsupported fence mode");
    auto &sync = as<sync_t>(s);
    sync.fenced = 1;
    sync.fence = xbg_legacy_last_fence();
}
API(nvnSyncWait, NVNsyncWaitResult, const NVNsync *s, uint64_t timeout) {
    LOCK;
    auto sync = as<sync_t>(s);
    if (!sync.fenced)
        return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
    if (xbg_legacy_wait(sync.fence, 0))
        return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
    return xbg_legacy_wait(sync.fence, timeout) ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED
                                                : NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
}
API(nvnQueueWaitSync, NVNboolean, NVNqueue *, const NVNsync *s) {
    LOCK;
    need(as<sync_t>(s).fenced, "Queue waiting on an unfenced sync");
    return xbg_legacy_wait(as<sync_t>(s).fence, UINT64_MAX) != 0;
}
} // namespace
extern "C" uint64_t nvn_new_serial() {
    static std::atomic<uint64_t> next{};
    return ++next;
}
extern "C" void xbg_record_command(cmd_t *c, const void *record, uint32_t bytes) {
    LOCK;
    need(bytes >= 8 && !(bytes & 7), "Invalid copied record");
    memcpy(allocate(*c, bytes), record, bytes);
}
int mng_nvn_initialize(const xbg_game_config *config, char *error, size_t bytes) {
    LOCK;
    if (ready)
        return -1;
    auto result = xbg_game_initialize(config, error, bytes);
    ready = result == 0;
    if (ready)
        host_config = *config;
    return result;
}
void *mng_nvn_get_proc_address(const char *name) {
    LOCK;
    auto it = functions().find(name ? name : "");
    if (it == functions().end())
        return nullptr;
    return it->second;
}
size_t mng_nvn_proc_names(const char **names, size_t capacity) {
    LOCK;
    size_t i = 0;
    for (auto &[name, _] : functions()) {
        if (names && i < capacity)
            names[i] = name.c_str();
        ++i;
    }
    return i;
}
int mng_nvn_shutdown() {
    LOCK;
    int result = xbg_game_shutdown();
    if (!result) {
        texture_snapshots.clear();
        program_snapshots.clear();
        command_owners.clear();
        objects.clear();
        host_config = {};
        ready = false;
    }
    return result;
}
