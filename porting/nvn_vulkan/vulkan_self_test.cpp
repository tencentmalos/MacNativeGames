// SPDX-License-Identifier: MIT
#include "nvn_vulkan.h"
#include "nvn_protos.h"
#include "tests/synthetic_spirv.h"
#include "engine/xbg_game_internal.h"
#include <filesystem>
#include <array>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <mutex>

void need(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
void vkcheck(VkResult r) {
    if (r != VK_SUCCESS)
        throw std::runtime_error("Vulkan call failed: " + std::to_string(r));
}
template <class T> T proc(const char *name) {
    auto p = mng_nvn_get_proc_address(name);
    need(p != nullptr, name);
    return reinterpret_cast<T>(p);
}
#define NVN(name) proc<NVNT_##name>(#name)
int mng_nvn_self_test(const char *directory, char *report, size_t report_bytes) {
    static std::mutex run_mutex;
    std::lock_guard lock(run_mutex);
    VkInstance instance{};
    VkDevice device{};
    bool initialized = false;
    void *storage = nullptr;
    std::string synthetic_directory;
    try {
        need(directory && report && report_bytes, "Missing self-test arguments");
        struct stat st {};
        need(stat(directory, &st) == 0 && S_ISDIR(st.st_mode), "Missing private shader directory");
        std::string pattern = std::string(directory) + "/mng-synthetic-XXXXXX";
        std::vector<char> path(pattern.begin(), pattern.end());
        path.push_back(0);
        need(mkdtemp(path.data()) != nullptr, "Synthetic shader directory allocation");
        synthetic_directory = path.data();
        std::array<std::array<uint8_t, 0x880>, 3> controls{};
        auto shader_bundle = [&](unsigned index, unsigned stage, const uint32_t *words,
                                 size_t bytes) {
            controls[index][0] = uint8_t(0xf0 + stage);
            char key[17]{};
            need(xbg_shader_control_key(controls[index].data(), key) == 0,
                 "Synthetic shader identity");
            xbg_shader_header header{0x31534758, 1, stage,           stage == 5 ? 1u : 0u,
                                     0,          0, uint32_t(bytes), 0};
            std::ofstream output(synthetic_directory + "/" + key + ".xgs", std::ios::binary);
            output.write(reinterpret_cast<const char *>(&header), sizeof(header));
            if (stage == 5) {
                xbg_shader_binding binding{
                    0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, 256, XBG_SOURCE_STORAGE, 0, 0, 0};
                output.write(reinterpret_cast<const char *>(&binding), sizeof(binding));
            }
            output.write(reinterpret_cast<const char *>(words), bytes);
            need(bool(output), "Synthetic shader bundle write");
        };
        shader_bundle(0, 0, triangle_spirv, sizeof(triangle_spirv));
        shader_bundle(1, 4, green_spirv, sizeof(green_spirv));
        shader_bundle(2, 5, storage_spirv, sizeof(storage_spirv));
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "MngNvnProbe";
        application.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &application;
        vkcheck(vkCreateInstance(&ci, nullptr, &instance));
        uint32_t n = 0;
        vkcheck(vkEnumeratePhysicalDevices(instance, &n, nullptr));
        need(n > 0, "No Vulkan devices");
        std::vector<VkPhysicalDevice> physical(n);
        vkcheck(vkEnumeratePhysicalDevices(instance, &n, physical.data()));
        VkPhysicalDevice gpu = physical.front();
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(gpu, &properties);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, nullptr);
        std::vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, families.data());
        uint32_t family = 0;
        while (family < n &&
               (families[family].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) !=
                   (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            ++family;
        need(family < n, "No graphics+compute queue");
        float priority = 1;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &queue_info;
        vkcheck(vkCreateDevice(gpu, &di, nullptr, &device));
        VkQueue queue{};
        vkGetDeviceQueue(device, family, 0, &queue);
        xbg_game_config config{};
        config.size = sizeof(config);
        config.physical = gpu;
        config.device = device;
        config.queue = queue;
        config.queue_family = family;
        config.fence_timeout_ns = 5000000000ull;
        config.shader_directory = synthetic_directory.c_str();
        char error[512]{};
        need(mng_nvn_initialize(&config, error, sizeof(error)) == 0, error);
        initialized = true;
        need(!mng_nvn_get_proc_address("nvnDefinitelyUnsupported"),
             "Unknown API must stay unresolved");
        NVNdeviceBuilder db{};
        NVNdevice nd{};
        NVN(nvnDeviceBuilderSetDefaults)(&db);
        need(NVN(nvnDeviceInitialize)(&nd, &db), "Device initialization");
        constexpr size_t bytes = 4u << 20;
        need(posix_memalign(&storage, 65536, bytes) == 0, "Pool allocation");
        memset(storage, 0, bytes);
        NVNmemoryPoolBuilder pb{};
        NVNmemoryPool pool{};
        NVN(nvnMemoryPoolBuilderSetDefaults)(&pb);
        NVN(nvnMemoryPoolBuilderSetDevice)(&pb, &nd);
        NVN(nvnMemoryPoolBuilderSetStorage)(&pb, storage, bytes);
        NVN(nvnMemoryPoolBuilderSetFlags)
        (&pb, NVN_MEMORY_POOL_FLAGS_CPU_CACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
        need(NVN(nvnMemoryPoolInitialize)(&pool, &pb), "Pool initialization");
        NVNtextureBuilder tb{};
        alignas(8) uint8_t texture_storage[144]{};
        memset(texture_storage + 8, 0xa5, sizeof(texture_storage) - 8);
        auto &texture = *reinterpret_cast<NVNtexture *>(texture_storage);
        NVN(nvnTextureBuilderSetDefaults)(&tb);
        NVN(nvnTextureBuilderSetDevice)(&tb, &nd);
        NVN(nvnTextureBuilderSetSize2D)(&tb, 64, 32);
        NVN(nvnTextureBuilderSetFormat)(&tb, NVN_FORMAT_RGBA8);
        NVN(nvnTextureBuilderSetStorage)(&tb, &pool, 0);
        need(NVN(nvnTextureBuilderGetStorageSize)(&tb) < 65536, "Texture storage bounds");
        need(NVN(nvnTextureInitialize)(&texture, &tb), "Texture initialization");
        for (size_t i = 8; i < sizeof(texture_storage); ++i)
            need(texture_storage[i] == 0xa5, "Opaque texture overwrote guest storage");
        alignas(8) uint8_t view_storage[32]{};
        memset(view_storage + 16, 0xa5, 16);
        auto *view = reinterpret_cast<NVNtextureView *>(view_storage);
        NVN(nvnTextureViewSetDefaults)(view);
        NVN(nvnTextureViewSetLevels)(view, 0, 1);
        NVN(nvnTextureViewSetLayers)(view, 0, 1);
        for (size_t i = 16; i < 32; ++i)
            need(view_storage[i] == 0xa5, "Texture view exceeded 16 bytes");
        alignas(8) uint8_t view_copy[16];
        memcpy(view_copy, view_storage, 16);
        view = reinterpret_cast<NVNtextureView *>(view_copy);

        NVNqueueBuilder qb{};
        NVNqueue nq{};
        NVN(nvnQueueBuilderSetDefaults)(&qb);
        NVN(nvnQueueBuilderSetDevice)(&qb, &nd);
        need(NVN(nvnQueueInitialize)(&nq, &qb), "Queue initialization");
        NVNcommandBuffer cb{};
        need(NVN(nvnCommandBufferInitialize)(&cb, &nd), "Command initialization");
        NVN(nvnCommandBufferAddCommandMemory)(&cb, &pool, 65536, 65536);
        auto record_clear = [&](int x, int y, int width, int height, const float *color) {
            NVN(nvnCommandBufferBeginRecording)(&cb);
            const NVNtexture *targets[] = {&texture};
            const NVNtextureView *views[] = {view};
            NVN(nvnCommandBufferSetRenderTargets)(&cb, 1, targets, views, nullptr, nullptr);
            NVN(nvnCommandBufferSetViewport)(&cb, 0, 0, 64, 32);
            NVN(nvnCommandBufferSetScissor)(&cb, x, y, width, height);
            NVN(nvnCommandBufferClearColor)(&cb, 0, color, 15);
            return NVN(nvnCommandBufferEndRecording)(&cb);
        };
        float background[] = {0.25f, 0.5f, 0.75f, 1};
        auto first = record_clear(0, 0, 64, 32, background);
        NVN(nvnQueueSubmitCommands)(&nq, 1, &first);
        NVNsync sync{};
        need(NVN(nvnSyncInitialize)(&sync, &nd), "Sync initialize");
        need(NVN(nvnSyncWait)(&sync, 0) == NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED,
             "Unfenced sync must not signal");
        NVN(nvnQueueFenceSync)(&nq, &sync, NVN_SYNC_CONDITION_ALL_GPU_COMMANDS_COMPLETE, 1);
        NVN(nvnQueueFlush)(&nq);
        auto wait = NVN(nvnSyncWait)(&sync, 5000000000ull);
        need(wait == NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED ||
                 wait == NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED,
             "GPU fence completion");
        NVNcopyRegion region{0, 0, 0, 64, 32, 1};
        std::vector<uint8_t> readback(64 * 32 * 4);
        NVN(nvnTextureReadTexels)(&texture, nullptr, &region, readback.data());
        for (size_t i = 0; i < readback.size(); i += 4)
            need(readback[i] == 64 && readback[i + 1] == 128 && readback[i + 2] == 191 &&
                     readback[i + 3] == 255,
                 "First GPU clear readback mismatch");
        float red[] = {1, 0, 0, 1};
        auto source = record_clear(8, 4, 16, 8, red);
        NVNcommandBuffer copy{};
        need(NVN(nvnCommandBufferInitialize)(&copy, &nd), "Copy command initialize");
        NVN(nvnCommandBufferBeginRecording)(&copy);
        NVN(nvnCommandBufferCopyCommands)(&copy, 1, &source);
        auto second = NVN(nvnCommandBufferEndRecording)(&copy);
        // CopyCommands must take a record-time copy, not emit a CALL to mutable
        // source storage. Poison source with an END record after copying it.
        uint32_t end[] = {0, 8};
        memcpy(reinterpret_cast<void *>(source), end, sizeof(end));
        NVN(nvnQueueSubmitCommands)(&nq, 1, &second);
        NVN(nvnQueueFinish)(&nq);
        NVN(nvnTextureReadTexels)(&texture, nullptr, &region, readback.data());
        unsigned red_pixels = 0;
        for (size_t i = 0; i < readback.size(); i += 4) {
            bool is_red = readback[i] == 255 && readback[i + 1] == 0 && readback[i + 2] == 0 &&
                          readback[i + 3] == 255;
            if (is_red)
                ++red_pixels;
            else
                need(readback[i] == 64 && readback[i + 1] == 128 && readback[i + 2] == 191 &&
                         readback[i + 3] == 255,
                     "Scissor corrupted background");
        }
        need(red_pixels == 128, "Scissor clear pixel count");
        // These are handwritten test shaders, never extracted game shaders.
        // Exercise offline SPIR-V loading and a real graphics pipeline/draw.
        NVNprogram program{};
        need(NVN(nvnProgramInitialize)(&program, &nd), "Program initialize");
        NVNshaderData shaders[] = {{0, controls[0].data()}, {0, controls[1].data()}};
        need(NVN(nvnProgramSetShaders)(&program, 2, shaders), "Offline shader assignment");
        NVN(nvnCommandBufferBeginRecording)(&cb);
        NVN(nvnCommandBufferSetScissor)(&cb, 0, 0, 64, 32);
        NVN(nvnCommandBufferBindProgram)
        (&cb, &program, NVN_SHADER_STAGE_VERTEX_BIT | NVN_SHADER_STAGE_FRAGMENT_BIT);
        NVN(nvnCommandBufferDrawArrays)(&cb, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
        auto third = NVN(nvnCommandBufferEndRecording)(&cb);
        NVN(nvnQueueSubmitCommands)(&nq, 1, &third);
        NVN(nvnQueueFinish)(&nq);
        NVN(nvnTextureReadTexels)(&texture, nullptr, &region, readback.data());
        for (size_t i = 0; i < readback.size(); i += 4)
            need(readback[i] == 0 && readback[i + 1] == 255 && readback[i + 2] == 0 &&
                     readback[i + 3] == 255,
                 "Offline shader GPU draw readback mismatch");
        NVN(nvnProgramFinalize)(&program);
        NVNprogram compute{};
        need(NVN(nvnProgramInitialize)(&compute, &nd), "Compute program initialize");
        NVNshaderData compute_shader{0, controls[2].data()};
        need(NVN(nvnProgramSetShaders)(&compute, 1, &compute_shader), "Compute shader assignment");
        auto *output = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(storage) + (2u << 20));
        memset(output, 0, 256);
        NVN(nvnCommandBufferBeginRecording)(&cb);
        NVN(nvnCommandBufferBindProgram)(&cb, &compute, NVN_SHADER_STAGE_COMPUTE_BIT);
        NVN(nvnCommandBufferBindStorageBuffer)
        (&cb, NVN_SHADER_STAGE_COMPUTE, 0, uintptr_t(output), 256);
        NVN(nvnCommandBufferDispatchCompute)(&cb, 64, 1, 1);
        auto fourth = NVN(nvnCommandBufferEndRecording)(&cb);
        NVN(nvnQueueSubmitCommands)(&nq, 1, &fourth);
        NVN(nvnQueueFinish)(&nq);
        NVN(nvnMemoryPoolInvalidateMappedRange)(&pool, 2u << 20, 256);
        for (unsigned i = 0; i < 64; ++i)
            need(output[i] == i * 17 + 3, "Compute storage writeback mismatch");
        NVN(nvnProgramFinalize)(&compute);
        xbg_game_stats stats{};
        xbg_game_get_stats(&stats);
        need(stats.submissions == 4 && stats.shader_loads == 3 && stats.draws == 1 &&
                 stats.dispatches == 1,
             "Submission count");
        NVN(nvnCommandBufferFinalize)(&copy);
        NVN(nvnCommandBufferFinalize)(&cb);
        NVN(nvnSyncFinalize)(&sync);
        NVN(nvnTextureFinalize)(&texture);
        NVN(nvnQueueFinalize)(&nq);
        NVN(nvnMemoryPoolFinalize)(&pool);
        NVN(nvnDeviceFinalize)(&nd);
        need(mng_nvn_shutdown() == 0, "Vulkan shutdown");
        initialized = false;
        free(storage);
        storage = nullptr;
        vkDestroyDevice(device, nullptr);
        device = {};
        vkDestroyInstance(instance, nullptr);
        instance = {};
        std::filesystem::remove_all(synthetic_directory);
        synthetic_directory.clear();
        snprintf(report, report_bytes,
                 "PASS: native NVN -> Vulkan on %s; 4 submissions, 3 offline test shaders, 1 GPU "
                 "draw, 1 dispatch, 64 storage words verified, 2048 RGBA pixels verified, 128 "
                 "scissored pixels, immutable CopyCommands, "
                 "GPU fence, readback and compact object/view canaries; BOTW "
                 "shaders/gameplay/presentation not tested\n",
                 properties.deviceName);
        return 0;
    } catch (const std::exception &e) {
        if (report && report_bytes)
            snprintf(report, report_bytes, "FAIL: %s", e.what());
        if (initialized)
            mng_nvn_shutdown();
        if (device)
            vkDestroyDevice(device, nullptr);
        if (instance)
            vkDestroyInstance(instance, nullptr);
        free(storage);
        if (!synthetic_directory.empty())
            std::filesystem::remove_all(synthetic_directory);
        return 1;
    }
}
