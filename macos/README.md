# macOS / Android 原生移植底座

Mac MoltenVK 与 Android 使用同一份 [Vulkan 默认策略](../porting/nvn_vulkan/RUNTIME_POLICY.md)。
启动脚本只传平台参数和显式覆盖，实际生效值通过 `renderer_schedule status` 核对。

macOS 提供两个明确的执行目标：`mng_botw_metal` 直接编译 MacNative 原有加载器、服务、窗口、音频、手柄和 Metal 后端；`mng_botw_vulkan` 复用同一组 Guest runtime，通过共享 NVN → Vulkan 渲染器与 MoltenVK 显示。Android 继续使用同一 Vulkan 渲染器。

这两个目标均只针对已验证的 Switch BOTW 1.6.0 元数据。不能拿 Wii U 地址或其他游戏的 patch 代替；私有模块、RomFS、存档与 shader 包留在仓库外。基础设施、Guest Patch 和平台呈现分别维护，尚未将所有 Metal 游戏 patch 迁入统一 manifest。

## 构建入口

使用 Apple Silicon 与支持原 Metal 代码的 Xcode 26；显式设置 `DEVELOPER_DIR` 与 `CMAKE_OSX_SYSROOT`，不改变全局 Xcode 选择。

```sh
cmake -S macos -B /absolute/build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DFOUNDATION_SOURCE_DIR=/absolute/foundation \
  -DVOLK_SOURCE_DIR=/absolute/volk \
  -DVULKAN_HEADERS_DIR=/absolute/Vulkan-Headers/include \
  -DD2R_SOURCE_DIR=/absolute/d2r-mobile-apple \
  -DMNG_BOTW_CONFIG_DIR=/private/build/botw160 \
  -DFMT_SOURCE_DIR=/absolute/fmt \
  -DFREETYPE_SOURCE_DIR=/absolute/freetype \
  -DIMGUI_SOURCE_DIR=/absolute/imgui
cmake --build /absolute/build --target mng_botw_metal mng_botw_vulkan mng_vulkan_probe -j8
```

Foundation、Volk、D2R 必须符合 `android/dependencies.json`；D2R 源码只读，适配在构建目录生成。Opus/libvpx 沿用 MacNative 的本机依赖。基础验证入口不需要游戏：`mng_vulkan_probe /private/writable-cache`。

渲染缺陷可使用 MoltenVK 自带的按需 Metal 抓帧：启动命令增加
`--gpu-capture /private/case/frame.gputrace`，到达目标画面后执行
`python3 macos/capture.py --pid <PID> --output /private/case/frame.gputrace`。
客户端先核对 debugbus PID/后端及该 PID 打开的私有 FIFO，仅请求一帧；输出路径必须未存在。
目录创建不代表捕获完成，客户端最多等 45 秒，看到 archive 的 metadata/index 后才报告发布。
再用 avp_study 的 `gputrace-cli import` 验证完整性、记录 manifest SHA 并查询 draw/shader。
不能把请求成功或目录存在当作捕获成功，也不能把开启捕获的运行用于性能 A/B。

运行时指定 `MNG_PROFILE_DIR`、`TOTK_PROFILE_DIR`，Vulkan 另需 `MNG_VULKAN_LIBRARY`（绝对 dylib 路径）和 `MNG_SHADER_DIR`（私有 XGS 目录），工作目录沿用原 MacNative 游戏根目录；参数为 `rewritten/botw160 boot`。默认手持 720p、关闭原 MacNative 自动 2K 输出。底座只验证 API 行为，实际分辨率还需读 `system_mode` 与 `renderer_targets`。

## 调试和分析

进程建立 `/tmp/mng-botw-PID.sock`，仅当前用户可访问。也可显式指定 `MNG_DEBUGBUS_SOCKET`，既有路径拒绝覆盖。传输在独立线程处理，读取请求有总时限；共享 command registry 使用缓存或 try-lock，不等待 Guest/renderer 大锁。

```sh
python3 macos/debugbus.py --socket /tmp/mng-botw-PID.sock --pid PID guest_modules
python3 macos/debugbus.py --socket /tmp/mng-botw-PID.sock --pid PID profiler_coverage
python3 macos/debugbus.py --socket /tmp/mng-botw-PID.sock --pid PID profiler_capture file 256 6
```

Vulkan 接 Foundation Litep CPU/GPU bridge。Metal 的帧事件由原 drawable 完成回调送到有界独立报告线程，过滤插帧；它支持 Guest/SVC CPU 记录，但当前 Foundation pin 没有 Metal GPU bridge，命令明确返回 unsupported。不能将此路径的默认 0 统计解释为无 GPU 工作。Mac Host TID 使用 pthread thread ID，不能把 Mach port 当 TID。

复用本机 `avp_study` 的工具，不在底座复制解析器：

- `avp/tools/instruments/reality-trace` 的 `doctor`、`record`、`native-read`、`cpu-hotspots`、`metal-timeline`：CPU 采样和实际 Metal 提交/GPU 区间。
- `avp/tools/gputrace/gputrace-cli`：查询 Metal 命令、资源、活动 shader；API 时钟不是 GPU 时长。
- 已验证使用 Xcode 16.3 的 Instruments reader、Xcode 26 编译器。采集时记录 PID、Mach-O UUID、驱动 SHA、模式和工具版本；停止后台构建，不同时录 CPU 与 GPU 两份 trace，也不直接拼接不同捕获的时间戳。
- `.gputrace` 使用 MoltenVK 自带的受控 capture 或 App `MTLCaptureManager`，不启动 AVP Simulator。独立 GPU replay 会改变排队时序，不作为原始性能证据。

## 并发边界

Guest 多线程、短锁命令录制、不可变命令副本、GPU 提交槽和独立 Present worker 已共享。原 Metal 的 `TOTK_ASYNC_REPLAY` 实现保留。Vulkan 呈现分为锁内准备/提交、锁外等待本次复制 fence、锁内退休三段；等待期间允许下一批命令翻译，源 Image 保留到真实复制完成。呈现路径已移除 QueueWaitIdle，交换链的 present semaphore 按 image 持有。翻译仍在调用线程同步执行，尚未接入 Foundation JobSystem。

GPU detail 计数、资源计数和 draw/pipeline 统计在解锁前取快照；成功呈现 marker 在复制 fence 完成后发出。不能把新帧的 draw 计入前帧，也不能在完成线程直接读取非原子的 renderer 状态。AcquireTexture 的票据到退休完成才满足；有界队列仍最多一个未完成呈现，避免扩大延迟与资源占用。

下一步先闭合提交快照：Guest UBO、间接参数、纹理和程序代、描述符版本必须随票据保留，再把无副作用准备工作放入 JobSystem。不能只复制 Guest 指针就异步执行。GPU 资源以真实 fence 退休。优化验收同时检查帧预算和跨线程区间并集，禁止将等待迁移报告成收益。细节见 `porting/JOB_SYSTEM.md`。

## MoltenVK 兼容补丁

`moltenvk/native-image-views.patch` 基于本机 MoltenVK fork `9e79070c`（1.4.2 rollup），在独立 worktree 验证；没有替换 Citron 手工测试的稳定驱动。应用前执行 `./fetchDependencies --none`，并保持依赖 revision 与该 tree 一致。

补丁闭合四处问题：

- BC + mutable/block-view/extended-usage 格式查询纳入兼容整数视图的 attachment/storage 能力。
- BC 原始整数视图的 Metal heap alias 按视图格式计算 texture usage，保留 RenderTarget。
- alias 视图保留真正的来源 texture 身份，避免把独立 heap alias 的 parent 当成来源后反复重建、释放在途 view。
- 3D BC 纹理的 2D 切片按 mip 的 depth 选择 slice；不能把深度切片当作 Vulkan array layer 计算 heap offset。同时验证派生的 offset、size 与 alignment。

共享 Vulkan 渲染器另有两项 MacNative 对齐，不属于驱动 patch：

- 不支持 `VK_EXT_custom_border_color` 时，沿用黑/白两次采样的线性组合。颜色通过每 draw 的 UBO 快照传递，变体只依赖 shader 与 binding 集合；改变 border 颜色不会重新编译 shader。只支持已验证的 float 非比较采样，未支持的 opaque-value 流明确报错。Android 原生扩展可用时沿用扩展，不保留额外 SPIR-V 副本。
- BOTW G-buffer 的相同纹理、mip、layer、format 重复颜色槽，只保留首槽。draw 必须证明重复槽的写掩码为零；清除重复槽转到首槽，完整和局部清除语义均保留。来源为 MacNative `rt_dup_of`，其上游归属为 MIT nxnative。

Metal validation 下的 Vulkan 探针已验证 32³ BC4 全部 32 个切片及局部清除、8,208 个 BC 原始 words、6,144 个自定义边框像素（含滤波边缘和颜色变化无新 shader）、6,144 个重复附件像素、GPU 读回、百万次对象复用、并发录制和控制句柄复用。depth bounds 在当前 MoltenVK 不支持，覆盖数为 0；不能把探针通过当作游戏画面验收。

## 启动与 shader 身份

推荐使用 `python3 macos/run.py --help` 的显式启动入口。Metal 除 `--build`、`--data-root`、`--profile` 外必须指定 `--backend metal --metal-assets /private/prepared-assets`；目录含 shader-seed、shader-libs、shader-service/totk_xlate 与 spirv-cross/spirv-cross。

构建产物 `mng-metal-assets.json` 和 seed manifest 的 chain、version、game、translator SHA 必须匹配。原 App 的旧 v6 shader seed 不能用于当前 v8；需要用当前 translator 重新准备，不能改 manifest 冒充同一产物。首次运行仍可能编译 Metal library，预生成 MSL 不等于预编译 metallib。新缓存按 chain 分目录保存。

Vulkan 使用 `--driver /absolute/libMoltenVK.dylib --shaders /private/xgs-bundle`，XGV 变体清单必须一起保留。启动后保存 `guest_modules`，并用 `lsof` / `vmmap` 验证进程实际映射的 driver；目录名不构成驱动证据。`--autoload` 默认关闭。

性能对照时两端都显式传 `--vsync off --fps-limit 120 --gpu-keepalive off`。
`--vsync off` 在 Metal 关闭 layer 的 display sync 与 display link，在 Vulkan 选择并核验 `IMMEDIATE`；driver 不支持时启动明确失败，不悄悄退回 FIFO。
`--fps-limit` 接受 30–240，只改变已启用实时 Guest Patch 的游戏节奏，影片仍为 30 FPS；120 不是无限速。
`--gpu-keepalive` 控制 Metal 专用保活负载，Vulkan 只接受 `off`。省略这些选项时保留原后端默认行为。
使用 Metal 的 `present_count` 比较完成次数；`frame` 是可能跳号的提交 ticket。

统一设置后的早期并发分析见 [2026-10-25 记录](CONCURRENCY_20261025.md)。反馈副本、依赖式 Barrier、附件与状态缓存、Metal 分层合并控制试验和仍未解决的翻译耗时见 [2026-10-26 对齐记录](CONCURRENCY_20261026.md)。

Mac Vulkan 启动器默认启用已验证的反馈连续段与 Barrier 调度，正常运行关闭旧逐 32 帧细粒度计时；显式 `MNG_FEEDBACK_SNAPSHOT=0`、`MNG_DEFER_FRAGMENT_BARRIER=0`、`DM_GFX_PERF_SAMPLE=32` 可用于回归 A/B。Android 新路径尚未实机验证。

本轮记录与后续性能工作边界见 [Mac 底座验证与 Instruments 分析](VALIDATION.md)。
