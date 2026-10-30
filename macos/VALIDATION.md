# Mac 移植底座验证与 Instruments 分析

本轮先建立同机的原生 Metal 与 NVN → Vulkan / MoltenVK 入口，再验证共同底座。Android 复用 Vulkan 的资源、命令、边框与附件处理；没有把 Mac 专用窗口或 Metal API 混入 Android。本文记录 2026-10-25 的实测边界，不能代替同场景性能比较。

后续统一关闭 VSync、120 FPS 上限、相同存档的对照，以及 CPU SyncWait 解锁、命令整段复制和附件反馈诊断，见 [并发与 pass 分析](CONCURRENCY_20261025.md)。该记录包含重新连接 Odin3 后的 Turnip GPU 探针结果；下文早期无设备、旧 PID 和旧采集结论保留为历史事实。

## 身份与隔离

- MacNative 源码基线：`a02228e8ec884dae66827de9a593bb9d9a31f0e5`，本地 `codex/android-zelda` 工作区有未提交改动。
- D2R Vulkan 参考固定为 `758f5d41509af5cd38b7eb934bc2334bdb3e3342`，保持只读，适配由 `prepare_engine.py` 写入构建目录。
- Foundation 固定为 `2f27d3bb41c1659d33b89905255fab2675fc0144`。
- 主机 Apple M4 Pro。构建显式使用 Xcode 26.6；Instruments 录制与 reader 使用已验证的 Xcode 16.3 runtime，不修改全局 Xcode 选择。
- BOTW Switch 1.6.0 `main` Build ID 为 `8E9978D50BDD20B4C8395A106C27FFDE`。Mac `guest_modules` 返回实际映射和模块 hash；旧 Mac patch 尚无统一 live receipts，接口明确显示 `unavailable_legacy_mac_policy`。
- 独立 MoltenVK worktree 基线 `9e79070c`，应用 `moltenvk/native-image-views.patch`。实测 dylib SHA-256 `8ea25172b62b40919bcb60d24c7d93970ac625921a38eb625e66d57f52e8ea8e`，UUID `12331E3A-07E0-3BE1-9BF9-EADB39C2733E`。Citron 原有 driver 和测试 App 未替换。

私有证据目录是 `/Users/bytedance/workspace/native-switch-study/macos-base-20261025`。原 App 存档未修改，测试使用复制出的独立 profile。模块、shader、存档与 trace 不放入 Git。

## 功能验证

`mng_botw_metal`、`mng_botw_vulkan` 与 `mng_vulkan_probe` 构建通过。Metal 原有 widen、block render、mid-clear、sampler-border 四项探针在 Metal validation 下通过。

共享底座的 12 项 CTest 通过，覆盖内存服务、Guest Patch、shader 选择、录制回放、手持/底座模式、GPU 时钟、帧节奏、显示节拍、呈现交接、保守范围集合和 border SPIR-V。Profiler 工具的 8 项 Python 测试通过；新增 SPIR-V 经 `spirv-val --target-env vulkan1.1` 验证。

Vulkan GPU 探针在 Metal validation 下通过 354 次提交，包含 BC 3D 切片、边框动态颜色、重复附件、别名 swizzle、RGB10A2SN、pitch、读回、命令复用和对象生命周期。详见 `probe20-copy.log`。另在仅 4 个 pipeline 对象的缓存下验证热对象不被误淘汰，16,384 个像素验证动态 blend 常量生效且不重建 pipeline。大于池的 buffer 描述保留原大小，缓存维护不越过 backing，溢出地址仍拒绝。命令式 Texture→Buffer→Texture 往返验证有效像素和行 padding，覆盖新接入的 CopyTextureToBuffer、CopyBufferToTexture、SetCopyRowStride、SetCopyImageStride。不支持的 depth bounds 明确为 0，未用跳过测试冒充通过。

实际 BOTW 的校验运行 PID 85497 已通过此前触发 abort 的 G-buffer 阶段，日志识别 `c1=c2`、`c3=c4` 且重复槽写掩码为零。记录过每帧 1,846 draws / 6 dispatches，无新 pipeline/shader 的窗口；到第 2,049 个成功 present 后保存证据并结束该测试进程，切换到关闭 Metal validation 的测量。后续帧仍可能按场景新建 pipeline，不能将一个无编译样本当作全程预热完成。

当前 Mac 手持模式报告：输出纹理分配为 **1920×1080**，呈现 crop 为 **1280×720**，internal scale 为 **1×**；同时存在 720p 和 1080p render passes。它不等于所有内部附件均为 720p。Android 的逐游戏分辨率 patch 尚需与 Mac 原有 patch 策略统一。

Android BOTW Debug APK 和独立 Vulkan probe 交叉构建通过（`apk-build130-copy.log`）。本轮 ADB 无连接设备，未安装新 APK、未声称新共享逻辑完成 Android 实机验证；原设备数据未清除。

## avp_study 工具实测

使用本机 `avp/tools/instruments/reality-trace` 的 `record`、`native-read`、`cpu-hotspots` 和 `metal-timeline`，没有复制解析器，也没有启动 AVP Simulator。CPU 与 GPU 各录 8 秒，录制串行，分析窗口取 1.5–7.0 秒。两份 trace 独立分析，不拼接时间戳。

原生 Metal PID 78627 的 Mach-O UUID 是 `75FBD9DB-EEB3-37E1-901A-CC618E9CED7F`，SHA 和配置保存在 `metal-capture-identity.json`。使用当前 v8 seed，未开 autoload，不能据此宣称已进入用户所指的游戏场景。

| 指标 | 原生 Metal 结果 | 解释范围 |
| --- | --- | --- |
| CPU 采样 | 15,154 样本，15.154 秒累计 sample weight | 跨线程采样估计，可超过 5.5 秒窗口，不是调用次数 |
| `nvn_encode_fifo_peek` leaf | 9.66% sample weight | 栈定位到 encode worker；源码为 WFE/SEV 加 semaphore 的分段等待，不能直接称为热点忙循环或据此删掉 worker |
| IOKit trap leaf | 7.60% sample weight | 需要与各 lane 和实际提交对应，单独采样不能分离 driver 工作与等待 |
| Compute GPU 区间并集 | 3.002386368 秒 / 5.5 秒 | 约 54.59%；按 channel 去掉嵌套重复 |
| Fragment GPU 区间并集 | 0.709637202 秒 / 5.5 秒 | 约 12.90%；不能与 Compute/Vertex 简单相加 |
| Vertex GPU 区间并集 | 0.754305513 秒 / 5.5 秒 | 约 13.71%；不能称为整个 GPU 的使用率 |
| 应用提交 | 11,095，均匹配 GPU 提交 | command-buffer 关联只在同 trace/run 内成立 |
| 应用区间开始到首个 GPU 区间 | p50 0.418458 ms，p95 1.472375 ms | 包含 CPU 编码，不是纯 GPU 排队延迟 |

`metal-timeline` 中 7,629 条提交缺少 encoding duration，保留 unknown；不把缺失值当作零编码开销。原生 Metal 的 drawable 完成节拍曾约 8.3 ms，但没有独立 Guest frame 身份，不能报告为 120 游戏 FPS。

### Vulkan CPU 捕获与缓存改进

`vulkan-cpu-record2` 的 PID 88592、Mach-O UUID `D4C543F3-B0E0-30F2-A099-15242783C259` 捕获成功。5.5 秒窗口共 7,912 样本，其中 693 个缺栈，保留缺失状态；录制日志中 Xcode 16.3 对部分 Xcode 26 DWARF 有读取警告。不能假定符号覆盖完整，也不能把未知 Guest 地址当成宿主函数。

已解析 leaf 包括 IOKit trap 7.67%、memmove 2.63%、`inline_commands` 2.10%、command 对象查询 2.01%、`cpu_access` 1.52%、`hash_memory` 1.19%。`nvnQueueSubmitCommands` inclusive 占 22.43%，与其 execute 子项重叠，不能相加。这些是采样权重，不是各阶段 wall time；后续仍需 Litep/调度闭合等待因果。

旧 Vulkan 运行的日志在 pipeline 上限 4,096 后，60 帧内出现约 10,108 次创建和淘汰。对照代码确认两个问题：fast/memo 命中不刷新 LRU；depth bias、blend 常量和线宽被放进静态 pipeline key。现已刷新热对象使用时间，并将这些标量转为 core Vulkan 动态状态，保留逐 draw 的实际值，与 Metal encoder 的处理边界一致。旧 warm list 原样保留，新列表使用 `vk-pipeline-list-mng-dynamic-v1.bin`，避免启动时重建旧标量组合；driver 二进制缓存继续复用。

修复后的已观察窗口约 1,266–1,330 个 pipeline，淘汰为零，多段 60 帧窗口无新建、无编译。这能证明缓存工作集与重建行为改善；运行中的场景仍变化，不能把日志中的 31–43 次呈现/秒当作严格 A/B 收益。

PID 86908 和 88592 随后都在同一范围请求退出：池大小 147,456，flush offset 0，请求 262,144 字节，flags 0x24。这与原 MacNative 对 BOTW 144 KiB 池、256 KiB buffer 描述的记录一致。共享实现已保留描述大小，将实际 cache maintenance 限于 backing；没有读取或写入未分配的尾部。旧失败运行与原始日志保留，不冒充成功的 GPU 捕获：`vulkan-cpu-record` 和 `vulkan-gpu-record2` 均记录 `recording_ok=false`、无 trace。

### Vulkan GPU 捕获

PID 91115 的 `vulkan-gpu-record3` 录制成功，`native-read` 导出 240,990 条 GPU 区间和 22,710 条应用记录。5.5 秒选定窗口内，Vertex 区间并集 1.273473403 秒、Fragment 1.104026827 秒、Compute 0.039788207 秒；分别约 23.15%、20.07%、0.72%。这些 channel 有重叠且不能覆盖所有 GPU 活动，不相加冒充总利用率。

窗口内 15,245 条应用记录中 749 条匹配 GPU，14,496 条没有匹配且缺少 encoding duration，明确保留 unknown。匹配部分应用区间开始到首个 GPU 区间 p50 为 1.478792 ms、p95 为 5.448625 ms，包含 CPU 编码，不能当作纯排队延迟。录制有 backdated signpost 警告，保留于 `record.log`。当前结果足以定位 GPU 空档候选，仍不足以完整归因全部等待。

该进程在捕获后通过 flush 范围检查，随后停在尚未注册的 `nvnCommandBufferCopyTextureToBuffer`。D2R 引擎已有复制实现，现已接上共享前端入口、不可变纹理引用和 stride 参数。保留失败现场，没有把旧进程误报为仍在运行。后续可执行文件按 SHA 冻结在证据目录 `binaries/` 下，避免再次构建覆盖采集对象。

## 修复后的持续运行与 Litep 验证

修复复制入口后的 PID 94907 使用冻结可执行文件，SHA-256 为 `c30e22d749790f866426e58754d6104d802a12374c3d30a6b8b3075a815e3fb1`，Mach-O UUID 为 `947770DC-E363-3713-A407-E64AE59A9B6E`；参数和 driver 身份见 `launch-copy.json`。已通过此前的 oversized flush 和 CopyTextureToBuffer 退出点，运行到至少 **18,804 次成功呈现**，采样时仍存活。该次样本为 41.31 ms/呈现、3,123 draws、15 dispatches、零新建 pipeline/shader；滞后八次呈现的 GPU 时间为 14.42 ms，不能直接与该次 CPU 样本配对。渲染 workload 与启动阶段不同，尚无具体游戏场景的用户或视觉核验，也没有独立 Guest frame token，不能称为固定场景游戏 FPS 或严格性能对比。

通过同一 PID 的 Unix debugbus，调用 Foundation resident ring 连续采集两份 6 秒文件，再使用 Litep MCP 原生 PROF reader 查询；未自行解析 PROF 或私有 SQLite 文件。

| 采集 | 基础 CPU/GPU | GPU detail（每 16 draw 取样，预算 256） |
| --- | --- | --- |
| 文件 | `litep-macos-basic/mng-botw-macos-94907-file-1792928415527037-1.prof` | `litep-macos-gpu/mng-botw-macos-94907-file-1792928454536387-2.prof` |
| SHA-256 | `a2f201f4dda4d7cfad694eb582905d224d21ad0aee14ad4a3903240cce7a694b` | `b0cc1bbba1f9c4283b73eb712912863292c5e56c63472f2028428d2dd83f59dc` |
| 首末 CPU 事件跨度 | 6,040.684 ms | 6,045.530 ms |
| 解码事件 / 成功呈现 marker | 7,535,261 / 166 | 6,427,612 / 145 |
| GPU begin / end | 831 / 831 | 34,307 / 34,307 |
| GPU 时间记录 / orphan time | 1,658 / 0 | 68,344 / 4 |

两份文件均为完整容器和 payload，无截断、无跳过 chunk、无 CPU scope 配对错误；均有一条 `dropped incomplete gpu zone` 和 22 条采集结束时仍开放的 CPU scope，保留边界缺失，不补零。GPU detail 的 rejected counter 为零，但这不消除 reader 报告的边界和 orphan time。两份记录均明确报告 `gpu_cpu_alignment=unknown`，因此这里只验证 GPU 插桩、传输和读取，不能由这些数据推算 CPU/GPU 重叠率。文件中 `in_game` 为调用方阶段标签，不能代替场景确认。采集后 GPU detail 已关闭，resident ring 恢复为未录制状态。

基础采集按线程查询发现：TID 579846 的 `NVN.FrontLock.Wait` 为 2,098 ms，另两线程分别为 1,323 ms 和 1,255 ms；呈现线程 TID 579854 的 `NVN.Fence` 为 1,535 ms（333 次），而 `Host.Present.WaitIdle` 为 85.50 ms（166 次）。提交线程 TID 579761 的 `NVN.Submit` 为 962 ms，`NVN.Draw` self 为 1,165 ms。它们是包含等待和抢占的经过时间，不能跨线程相加、不能当作 CPU 使用率，也不能仅凭聚合确定锁持有者。现有证据支持优先关联 Fence 所属提交、前端锁持有区间和 engine 锁；不支持把 WaitIdle 单独认定为主要瓶颈。结构化查询结果保存在 `litep-integration-report.json` 和 `litep-present-scopes.json`。

可离线复验的 Android 产物为证据目录中的 `botw-130-copy.apk`，SHA-256 为 `16cd6cfdac8d03470f845ecdfd6ac2da2731f741615c285ecab6ab1d1e89b8d9`。当前仍缺少 Android 实机回归。

## 并发改进边界

已具备两个后端的运行、身份和采集入口，以及共享资源生命周期回归。下方后续改造已移除呈现路径的 `vkQueueWaitIdle`，复制 fence 等待不再持 engine 锁；Vulkan 翻译仍同步，普通提交槽退休仍有锁内 Fence 等待。该历史检查点尚未接入 Foundation JobSystem；2026-10-26 已补 PSO 准备队列，Draw 翻译仍同步，CPU/GPU 饱和目标尚未验收。

下一步按证据拆解 Guest 生产、CPU 翻译、driver 编码和 GPU 区间，再决定并发改动。异步提交必须保存 UBO、间接参数、资源代和 descriptor 版本，GPU 退休由真实 fence 驱动；先修正所有权，不能只把 Guest 指针交给 worker。具体契约见 `porting/JOB_SYSTEM.md`。

## 后续改造：呈现准备、完成与退休分离

源参考为 MacNative `nvn_mtl.m` 的 present ticket / texture 保留与 AcquireTexture 归还机制。共享 Vulkan 改造不异步读取 Guest 指针：先在 engine 锁内解析纹理、保留真实 `shared_ptr<Image>`、提交渲染和复制，再释放锁等待自己的复制 fence，最后重新持锁释放资源。下一批命令可以在等待期间翻译；VkQueue 的 submit/present 仍在 engine 锁下执行，避免并发访问同一队列。

交换链 image 分别拥有一个 present-ready semaphore；acquire semaphore、复制 command buffer 与 fence 在上次复制完成后复用。复制 fence 不能证明 WSI 已消费 present semaphore，所以没有在 fence 完成时销毁或跨 image 复用该 semaphore。这遵循 [Khronos 的交换链 semaphore 复用说明](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)。当前仍未实现交换链 resize/recreate 与独立呈现队列，AcquireImage/QueuePresent 仍可能在锁内阻塞。

`SerialHandoff` 有界准入和“准备取得锁后才返回”的契约保留，票据只在退休后完成。新增测试将完成阶段人为阻塞，验证 engine 锁可取得、第二个任务仍受有界准入限制、旧 image 票据不会等待新 image、闭包资源不会提前释放，关闭会排空已接收工作。

真实 GPU 探针通过 **356 次提交**（`probe21-present-split.log`，Metal validation 开启），新增：呈现复制尚未完成时，另一个线程提交独立 clear 并 Finalize 源纹理，随后读回全部 **2,048 个绿色像素**。这验证复制源的保留和翻译推进，不代替完整游戏画面验收。12 项 CTest 全部通过；Mac Metal/Vulkan、Android APK 和 Android Vulkan probe 均构建通过。

GPU detail 的计数和状态缓存与 renderer 同属锁内阶段；`preparePresent()` 在解锁前取数并归零，`present(token, true)` 在 fence 完成后只发 marker。overlay/debugbus 使用准备阶段的不可变统计快照，避免把后续翻译的 draw/pipeline 计入前帧。

新版 Mac Mach-O SHA-256 `72a706c29bfe6bda4a26f54114a2abf0505119569ac9ed7fc2a465076d9be33d`，driver 仍为前述独立 MoltenVK。PID 27197 在 Metal validation 下完成至少 10,266 次呈现，后按记录重启到关闭验证层的 PID 29881。旧 PID 94907 在替换前已超过 197,000 次呈现，保留了末次状态和一份 6 秒基础 PROF，随后有意终止，不能把它继续算作新版的长期证据。

重启后自动读档曾停在 DLC 页面（一次导航核对，非性能采样）。该页面约 115–118 draws、零 dispatches，与旧进程约 3,000 draws 的记录不同；即使观察到约 16.7 ms 的呈现间隔，也不能报告为游戏场景提速。之后用户手动进入场景，补采了下方记录。`Host.Present.Prepare`、`Host.Present.CompleteUnlocked`、`Host.Present.CopyFence` 和 `Host.Present.Retire` 为新的阶段桩点；没有把复制等待移到别处就当作整体帧预算下降。

本次 Android 产物 `botw-131-present-split.apk` SHA-256 为 `2af0daa2c3f19055d6fda31969947538b63ec70177b67e888fbe60e95111f583`。ADB 仍无设备，本次只完成交叉构建，未安装验证。

### 用户进入场景后的 Litep

PID 29881 的 `litep-after-present-split-game/…-2.prof` SHA-256 为
`4abdb8e78c87cbe9dccd26c3589a976cace999307b6ff6db9f75fa68819b85ef`。
原始 ring 含较早的前导事件，不能用全文件 321 秒跨度归一化这次 6 秒采集。
通过 Litep 原生 reader 明确裁剪到首末 present marker，窗口为 5,983.007 ms；
端点采用半开区间，包含 194 个 marker。完整 payload、零跳过 chunk、零 CPU scope 配对错误；
仍有一条不完整 GPU zone、6 个 orphan GPU time，CPU/GPU 对齐未知。
结构化结果见证据目录 `text-gameplay-litep-report.json`。

未裁剪的完整 scope 聚合中，present 的 CopyFence 为 1,352 ms，准备阶段 113 ms；
FrontLock.Wait 在不同线程分别约 1,866 / 1,220 / 1,022 ms，另两条线程 Fence 约 642 / 462 ms。
它们不能跨线程相加，且仍含等待和抢占。菜单捕获中没有测到 Guest 翻译与 CopyFence 的重叠；
本轮并发能力由受控 gate 测试验证，尚不能宣称实际游戏整体加速或 CPU/GPU 饱和。

## 游戏文字缺失：采样视图类型适配

用户报告游戏内文字不显示后，先保留 PID 29881 的日志和 Litep。
native-debugger 外部挂接在启动 Debug Host 前被拒绝，错误为
`mac_attach_exception_chain_unproved`；未使用不安全覆盖。会话已导出并 cleaned，
没有实际暂停或挂接游戏。随后明确切换诊断运行 PID 38589，程序和 driver 与上一版相同，
仅增加 MoltenVK 按需 Metal 捕获。原进程的保存证据与结束记录均保留。

`text-missing.gputrace` 的 metadata 报告 1 帧，avp_study 原生导入成功：
generation `gen-5e47207c357290865abafb7f`，manifest SHA
`22a76f54bb359fa648e7e3a748aae513b88e574d996c6beae9fe4166fe0fe801`。
第一次在 index 尚未发布时导入失败，保留失败 journal；待发布后重新导入成功，未覆盖原件。
本机 replay 私有 ABI 未通过工具的 UUID 门控，未绕过；以下是离线 API/资源链与实机视觉复验，
不是逐像素 replay 的结果。

具体证据链：`cmd-28023` 使用 `obj-03328`；其 fragment MSL `pl-02263`
声明 `texture2d_array<float>`。fragment argument buffer 绑定 `obj-03699 + 7744`，
最近一次 capture restore 对应 `pl-09666`，纹理槽 id(4) 中为 GPU resource ID 32。
捕获工具的 `TextureGPUResourceID` 将其映射到 `0x78ffc07480`；创建 API 显示该视图为
`R8Unorm / 2D / ONE,ONE,ONE,R`。这与 shader 要求的数组视图类型不匹配。
buffer 内容是 capture restore 快照，不能单独称为任意事件时刻的 GPU 内容。

MacNative `nvn_mtl.m` 的 `tex_type_view` 及 sampled texture 绑定分支已经对 UI 字形图集
处理 2D 与 2D-array 的互转。共享 Vulkan 现按离线 shader binding 的维度/array 标志，
创建匹配的采样视图；互转时使用所选子资源的首层，保留 mip、格式和 swizzle。
image lookup cache 的 key 同时加入 shader image flags，避免同一 texture handle 被不同签名
使用时命中错误视图。没有更改字体文件、shader 源语义、存档或并发等待策略。

新增 R8 字形图集 GPU 测试覆盖 2D 与双层 array backing、两种 shader 交替、非零越界层索引
钳制、Alpha 映射和缓存复用，共 16,384 像素。`probe23-glyph-view.log` 在 Metal validation 下
通过 364 次提交及其余既有检查。早期仅采第 0 层的旧实现测试也能通过，不能把它作为有效负例；
最终测试补充了非零层索引。Mac Vulkan、Android APK、Android Vulkan probe 构建通过。

实机新进程 PID 43514 使用冻结二进制 SHA-256
`50f61df1831bea29d1fa0ad5504b3cae8c18a7fed5d9e2ccb3c460ac29cff055`，
driver 未变，抓帧和 Metal validation 已关闭。`text-before-fix.png` 与 `text-after-fix.png`
在同一存档的复苏神庙入口核对：修复前只有 L / ＋ / R 图标，修复后显示
“移动 / 操作说明 / 视角操作”。这是本缺陷的画面验收；不声称所有游戏页面均完成验证，
也不使用两张图上的 FPS 数字做性能比较。Foundation HUD 仍有问号字体的问题，属于另一条字体路径。

Android 产物 `botw-132-glyph-view.apk` SHA-256 为
`84cf29acd8549d4902a24654206125c9acaff59e5bdb5068fa0c834ce6170c91`。
ADB 无设备，未安装，Android 画面效果尚待实机验收。

## 同场景性能复采与纹理退役修正

2026-10-25 后续已完成复苏神庙同存档的 Metal / Vulkan CPU、GPU 对照及修复后复采。
见 [BOTW 同场景性能报告](PERFORMANCE_20261025.md)。本轮移除 TextureFinalize 的整队列等待，
保持 fence 延迟回收；短窗口呈现率约 33 → 39–40 次/秒，仍有全局锁和同步翻译瓶颈。

## 2026-10-26 Pass / 翻译对齐

最新证据与完整范围见 [Metal / MoltenVK 对齐记录](CONCURRENCY_20261026.md)：Vulkan 约 465 → 130–131 Pass，Metal 关闭分层合并后约 127–128；382 次 Vulkan probe 提交通过。Translate 仍约 10.73 ms，对照 Metal 5.30 ms，不能称为性能对齐已完成。

## 2026-10-26 PSO 准备队列

共享 Vulkan 已接入 Foundation WORK_JOB，任务按 key 去重并有序领取，完成 Mac/Android 构建和实际 GPU 回归。Odin3 同样 2255 个 PSO 的 2 / 4 worker 准备窗口为 60.6 / 41–47 ms；稳定场景仍约 22.5 FPS。原始 Litep、安装身份、默认值和未完成边界见 [PSO 验证](../porting/nvn_vulkan/PSO_WARMUP_20261026.md)。
