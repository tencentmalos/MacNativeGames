# BOTW 原生移植的优化迭代方法

当前先推进 BOTW；Android 与 Mac 共用移植底座，逐游戏差异进入 Guest Patch。遇到性能或正确性问题，优先核对同一仓库中已运行的 MacNative 实现，再决定 Android 侧方案。

## 每轮迭代

1. 用 Foundation debugbus 核实进程、模块 Build ID、驱动和最近成功呈现。正常运行使用低开销 Litep 桩点；HLE 全量计数和细粒度 GPU 查询只在短窗口开启，采集后关闭。
2. 从实际帧窗口识别热点，记录次数、逐帧覆盖和等待。CPU scope 是经过时间，不直接视作 CPU 执行时间；未覆盖的 GPU 查询区间不视作 GPU 空闲。没有持续编译证据时，不将低帧率归因于 shader 动态编译。
3. 回看 MacNative 对应对象、录制、回放、缓存和队列生命周期。优先复用它的语义与所有权设计；Metal 与 Vulkan 的同步实现需要分别验证。Mac 的 FPS 不能作为 Android 优化收益。
4. 写明可检验的假设、最小修改、正确性风险与验收条件。先执行合成压力测试和 GPU 回读，再以相同读档和输入流程比较 APK。保留旧 APK、PROF、sidecar、哈希及进程身份。
5. 性能改善必须同时满足正确性和长期运行条件。长测观察累计初始化与当前保留量，不能只抬高上限；等待移动到另一线程也不算优化。保留尚未解决的问题，开始下一轮。

## 当前轮：对象生命周期与 sampler 比较

旧实现把纹理放进进程级宿主对象表。BOTW 会按值复制和反复初始化纹理；为了保护不可观测的副本，Finalize 没有删除记录，最终触发 1,048,576 条的对象保留上限。

Mac 侧参考：

- `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_obj.h`：紧凑纹理、sampler 与 builder 值对象。
- `games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c`：TextureInitialize、TextureFinalize 和 tex_pin；新初始化序号、重新初始化时释放旧登记、录制快照。
- `games/totk/third_party/d2r_gfx/gfx/nvn/totk_nvn_ext.c`：SamplerCompare 直接比较参数。

Android 方案：

- 将纹理的宿主表示压缩到已验证的 128 字节 Guest 存储内，调用 Vulkan 后端时展开成其较大的结构。副本之间不共享可变对象记录；录制命令仍保留不可变快照。
- sampler、buffer 与无所有权的 builder 按值存储。SamplerCompare 无需全局锁、对象表或 RTTI 查询。
- swizzle 编码进纹理序号低位，视图使用同一代号的不同分量映射，避免另建历史对象表。Finalize 和重新初始化释放该代号的图像登记；物理存储另行拥有 GPU 图像，内存池释放或无身份引用且闲置超过 600 次提交时，才进入提交槽退休队列。
- 虚拟池变更检查渲染器实际持有的图像范围；不再扫描历史纹理对象表。保留 GPU 已写数据必须显式合并的校验。
- `renderer_resources` 使用 try-lock 返回当前对象、纹理快照、命令字节、图像登记及累计初始化数量。忙时返回可重试状态，Android Service 不等待渲染完成。
- Litep 同时记录 `Native.HostObjects`、`Native.TextureSnapshots`、`Native.CommandBytes`、`Native.ImageIdentities`、`Native.PhysicalImages`、`Native.TextureInitializations`。

验收包括超过旧上限的纹理/sampler 初始化和销毁、宿主对象数量不增长、按值复制与存储边界、延迟提交、视图 swizzle、GPU 像素回读，以及实际游戏跨越旧故障窗口的持续运行。压力测试不代替长时间游戏验证。

## 当前优先级：首屏

先收敛首屏的节拍问题，再回到场景内 Snapshot/Descriptor 和同步优化。APK105 已默认启用有界呈现交接，同一实现的 APK104 在 60 秒验证中约 29.50 FPS（旧版约 20 FPS），仍保留少量长帧问题；Mailbox 和显示回调实验均默认关闭。首屏无持续 shader 构建证据，已定位 Present 人为等待与 GPU/Guest 时钟起点两类独立问题，结果见 [TITLE_PERFORMANCE.md](TITLE_PERFORMANCE.md)。不能把校时的正确性修复误写成帧率收益。

## 场景内候选（首屏完成后）

根据新采集结果重新排序，当前候选是 NVN Draw 的宿主处理、Guest 条件等待及 CoreReacquire。先检查 Mac 的命令录制/回放和线程调度，再决定是否减少同步或缓存重复状态；不同时改动多个无法独立归因的路径。

APK90 追加的 CPU detail 捕获中，连续 SDK 帧 147–154 每帧约 27,282 次 Snapshot、3,700 次 Descriptor、13,641 次 Image 和 3,654 次 Pipeline 查找。高频细分插桩把该小窗口均值抬到 94.36 ms，不能替代基础窗口的 75.56 ms 或直接相减归因。下一步优先对照 Mac 的提交时快照、状态复用与引用所有权，减少重复的快照/描述符处理；Pipeline 查找不是驱动动态编译，不能因调用次数多就宣称 shader 编译是瓶颈。

本次按范围同步 A/B 的完整帧仅减少约 6.81 ms，主要等待从 Fence 移到 Present，且游戏动态分辨率变化。保留诊断入口，稳定默认仍为 preflight=1；后续应在固定场景、明确的实际附件尺寸下验证 CPU/GPU 可见性，再决定是否启用。

本轮架构对齐、闪黑回归、音频供给和分辨率验证见 [ARCHITECTURE_ALIGNMENT.md](ARCHITECTURE_ALIGNMENT.md)。CommandBufferFinalize 现在删除独立录制器宿主记录，EndRecording 产出的命令副本仍由 handle 所有；sync 改为按值存储。资源快照按 command、pool、descriptor pool、program、sync、queue、window、other 分类，便于检查残留增长。

## 2026-10-25：60 FPS 补丁之后的场景定位

APK106 的首屏 6 秒 Litep 有 360 个完整成功呈现帧，平均 16.655 ms，P50/P90 16.645/18.604 ms。进入存档场景后，独立基础采集为 76 帧，平均 77.879 ms（12.84 FPS），主 NVN 线程的互斥预算为 GuestCondition 29.302 ms、Draw 26.132 ms、Fence 15.878 ms、Other 自身 3.108 ms。这里的耗时包含调度和阻塞，不能当成 on-CPU；GPU 工作不是上述 CPU 项的可加总部分。

同 APK 关闭 `read_preflight` 的诊断窗口有 90 帧、平均 66.051 ms（15.14 FPS）；Draw 22.548 ms，GuestCondition 28.095 ms。GPU 等待主要移到了呈现线程，因此没有把“主线程 fence 为零”当作等待消失，也没有将这个开关设为默认。最终版继续保留 `read_preflight=1`，避免范围检查在已经取得 renderer 引用之后触发提交/退休的生命周期风险。

HLE 采集进一步发现 `nvnMemoryPoolMap` 36580 次调用累计 2889.636 ms，而它只是取得稳定映射地址。Android 的对象注册表共用了 `front_mutex`；该锁也覆盖整个同步 Vulkan 翻译，导致持有 Guest 虚拟核心的只读查询被渲染阻塞。Mac 对这类值直接读取，没有整队翻译锁。修复采用独立对象注册表锁，只保护句柄查找、创建、销毁和计数；取得它时不调用 engine、不反向取得 front_mutex。此处描述 APK108 的阶段实现；APK111 起命令录制另用独立锁，提交和真实资源同步仍保留前端顺序。debugbus 的资源统计仍全部 try-lock，忙时返回可重试状态。

APK107 阶段同时移除命令句柄替换/Finalize 中的整队 `xbg_legacy_finish`。这些是 CPU 解释器的字节，GPU 不直接读取；同步 Submit 在 `front_mutex` 下读完，注销命令范围还会取得 engine 锁。纹理快照和 GPU 物理存储继续由各自所有者及 fence 保留。这与 Mac 的 BOTW control-handle 复用规则一致，不允许将同样做法扩展到纹理或内存池的回收。

实机合成测试增加：持有翻译锁时另一线程能够完成实际内存池 getters；同一 control handle 连续复用 16 次并在最终 GPU 等待前 finalize，回读验证 2048 个条纹像素。仍运行既有 1048580 次对象 churn、66000 次纹理录制回收和物理/虚拟别名等回归。`probe107-pass.log` 曾来自未重链接的旧独立 probe（Gradle APK 目标不负责该可执行文件），不作为新检查证据；显式构建 `mng_vulkan_probe` 后的 `probe108-pass.log` 才含新增检查和 305 次提交。


## 2026-10-25：录制并发、虚拟核心等待与管线查找

在相同 Odin3、720p handheld、1×、60 FPS 补丁及 `read_preflight=1` 下，分步基础窗口如下。场景使用同一读档流程，动态分辨率、温度和即时状态没有严格锁定，因此表格是迭代观测，不将每一步的差值全部归因于代码。

| APK | 实现 | 完整帧 | 平均帧时间 | 对应 FPS |
| --- | --- | ---: | ---: | ---: |
| 106 | 60 FPS 补丁后的场景基线 | 76 | 77.879 ms | 12.84 |
| 107 | 移除 CPU 命令退休时的整队 Finish | 77 | 77.885 ms | 12.84 |
| 108 | 对象表查询锁独立 | 83 | 71.460 ms | 13.99 |
| 111 | 录制与同步翻译分锁，提交保留不可变副本 | 81 | 73.204 ms | 13.66 |
| 114 | ABI 常量无翻译锁、Flush/Invalidate 等待归还虚拟核心、精确管线状态 memo | 88 | 67.507 ms | 14.81 |

107 没有测得帧率收益；111 相对 108 的短窗口也没有改善，不能单凭并发测试通过就宣布游戏更快。111 的主 NVN 线程每完整帧 Draw 26.369 ms、GuestCondition 24.364 ms、Fence 16.060 ms，表明录制分锁后仍有其它串行依赖。

随后 HLE 诊断定位 `nvnMemoryPoolFlushMappedRange`（709 次，累计经过时间 2719.712 ms）和 `nvnDeviceGetInteger`（321 次，1678.759 ms）等待翻译锁。114 对固定 ABI 常量直接返回；真正需要 renderer 的属性查询才进入锁，并先释放 Guest 虚拟核心。Flush/Invalidate 也先归还虚拟核心，释放宿主锁后重获。对照 Mac 后保留了 Flush 的原翻译顺序：Mac 会按范围处理镜像草地遮罩回写，尚不能宣称 Android 只需一个 CPU fence。

管线 memo 使用固定 D2R 引擎已有的精确状态键，与其 Android host 的默认选择一致；新增的 depth-clamp、line-width、raster-discard 位于 State 的键覆盖区间，shader 变体解析也在键建立前完成。该路径只复用相邻状态一致的 VkPipeline，不跳过 draw、不在后台编译。实机日志每帧约 2916 次命中、721 次未命中，抽样 Pipeline 处理约 1.48 ms（111 约 3.5 ms）；只把此作为热点变化证据。开发版可用 `debug.mng.pipeline_memo=0/1` 做单项对照，配置写入 PROF 身份。

### 命令所有权与回归

APK111 起，Submit 在短暂 command 锁内解析并保留全部 `shared_ptr<CommandCopy>`，随后离开录制锁进行同步翻译。副本包含字节和纹理引用；复用控制位置、Finalize 公开 handle 不改变在途提交。独立 CPU 区间表替代 renderer 内存池登记，每次 Submit 清空解释器区间缓存。录制路径不得在 command 锁内进入 engine；图像/内存池 GPU 生命周期没有因此缩短。

实机 probe114 使用同代 Turnip 和 `DM_GFX_PIPE_MEMO=1`，通过 305 次 GPU 提交、2048 条纹像素检查、1048580 次对象 churn、66000 次纹理录制回收、别名和原有像素回归。并发检查覆盖持有翻译锁时的对象 getter、ABI 常量、完整录制与状态绑定；额外保留旧提交副本，复用/销毁相同控制句柄后检查旧字节和范围仍有效，最后一个 pin 释放后区间才退出。它不代替长时间游戏画面验收。

### 原始证据

原始目录仍为本机 Git 外的 `porting-20261024-lifetime`：

- APK111：`edda1b82c445a0747811440baa5e0391c3ec79415079a4536d229f9f727945c5`，PID 26554；基础 PROF `b1856029586a2332f3cc689ed5f642f975d555c270493e1268b021399eca9c3c`。
- APK114：`0e65b652a76bf9108e46fafe3c4709952d251fca1475d99906a212d3980c2378`，安装后的 base.apk SHA 已回读一致，PID 29957；6 秒基础 PROF `fbef07dee999a93616d7feef4d57f3e42a4ae8800d0a5b9fccfc5938082fdfa4`。
- 111 detail/HLE PROF：`6e9e25db719ea80a3320a7618cea25c72b3956a15f06a43d7fd2c6022450809c`、`3ad2e1d7bfbce79c5dc2d0636cee2618efbfdfebe820d03f056bde70cbf052fa`。两者实际请求均为 6 秒（临时脚本名的 3/2 不代表请求时长）；聚合器报告 capture_window_verified=false，有跨边界长等待，因此这些累计值仅用于诊断，不作为 FPS 或逐帧预算。
- Foundation JobSystem 的实际接入状态、Mac/Azahar 对照及后续任务所有权契约见 [JOB_SYSTEM.md](JOB_SYSTEM.md)。


114 的 6 秒完整帧预算：Draw 独占经过时间 23.103 ms（3646.26 次/帧），GuestCondition 22.380 ms，Fence 15.957 ms（17.81 次/帧）；呈现线程 WaitIdle 0.467 ms，未出现把主要 Fence 等待搬到呈现线程的现象。6 秒窗口 P50/P90 为 66.196/76.386 ms，无丢块或 CPU 配对错误，保留一条边界 GPU zone 不完整诊断。

114 的 2 秒 HLE 复核中，原先被阻塞的线程对 DeviceGetInteger 调用 121 次、累计 0.038 ms；其它两线程分别为 241 次/0.074 ms、86 次/0.024 ms。该诊断仅验证查询锁已解除，不能与不同长度的旧 HLE 窗口直接比较总耗时。Flush 仍有累计等待，区别是等待期间归还虚拟核心；尚未完成范围相关回写优化。

采集结束后 HLE/detail/fine-GPU/ring 均关闭，保留游戏场景运行。一次资源快照为 host_objects=10787、command_handles=40085、command_bytes=26740368、texture_snapshots=590、physical_images=773，累计 texture_initializations=447768；此刻未达到百万初始化长测阈值，不宣称替代上一轮长期验证。renderer_targets 的交换纹理分配为 1920×1080，有效 crop 为 1280×720；system_mode 为 handheld，operation/performance 实际查询分别 2/1 次，内部倍率 1×。


### 20 秒连续帧复核

同一 APK114/PID29957 的 `candidate114-game20/basic` PROF SHA 为 `efa2609c063ed9a64d231340b152ea17d4fdf2ae54fd60718d248a11aece5035`。文件完整、无丢块、无 CPU 配对错误，保留一条边界 GPU zone 不完整诊断。首个 SDK frame 116 跨越停止 HLE 捕获和重新开始基础捕获之间的未录制时间，表面长度 3319.347 ms；其两端 `Native.Present.Token` 从 3115 跳到 3165，证明期间实际完成了 50 次呈现，不能当作一个游戏长帧。该问题符合 [PROFILING.md](PROFILING.md) 已记录的 stop/start 首 marker 边界。

仅采用连续 SDK frames **117–419**，共 **303 帧、19.968 秒**，平均 **65.901 ms（15.17 FPS）**，最长 101.053 ms。包含旧 marker 会误报 76.603 ms；没有按 FPS 好坏挑选帧，唯一排除项由捕获边界和呈现 token 跳变共同证明。最终更长窗口仍约 15 FPS，不能宣布场景低帧率已经解决。


## 2026-10-25：范围预检和渐进 GPU 提交

本轮目标是持续减少人为串行等待，直到实测关键路径由 CPU 执行或 GPU 工作主导。以下中间版本均未达到该目标；减少等待次数、把等待搬到呈现线程不算完成。

| APK | 改动 | 完整基础帧 | 平均帧时间 | FPS |
| --- | --- | ---: | ---: | ---: |
| 114 再采 | 本轮基线 | 91 | 65.827 ms | 15.19 |
| 116 | Draw/Dispatch 按实际读取范围预检 | 93 | 63.947 ms | 15.64 |
| 117 | 清屏、复制和 resolve 也按范围预检 | 78 | 76.389 ms | 13.09 |
| 119 | 在目标绑定边界提前分批提交 GPU | 100 | 59.290 ms | 16.87 |
| 120 | Builder 参数接口移除翻译锁；增加锁等待桩点 | 104 | 57.654 ms | 17.34 |

相同设备、读档流程、720p 手持、1×、60 FPS Guest Patch 和 Turnip；实时场景状态、动态分辨率和温度并未完全锁定，不把每一差值全部归因于单项代码。APK115 在空 Dispatch 上提前验证未绑定 storage，触发断言；116 已将无工作量命令保留为原来的 no-op，并增加回归。失败版本不计入性能结果。

范围预检在取得 command buffer、descriptor、Image 引用之前检查潜在 CPU 读取。GPU 输出全覆盖且对齐时继续采用 GPU view；最新部分写入不能被较旧的完整写入遮蔽，部分覆盖仍等待回写。合并 storage 别名的算法与 descriptor 构建保持一致。若遗漏范围导致 draw 内 cpu_access 试图同步，立即报错，防止提交/退休后继续使用失效引用。空 draw/dispatch、8 位索引 CPU 扩展、图像导入和报告内存分别遵守原语义。

117 每帧只剩约两次 Vulkan 提交，但 GPU 直到整帧翻译完成才得到工作。主线程 Fence 几乎为零，呈现线程仍等待约 12–15 ms，完整帧反而更慢。119 参考 Mac 的 nvn_mtl.m 中 chunk_maybe_split / mtl_sub_split，在显式 OP_SET_RT 边界、完成前一操作后提交已有批次；默认阈值为 1024 个 GPU 操作。保留两个提交槽、跨槽 GPU view 引用及 fence 退休规则；中途提交不提前完成整个 Guest 提交票据。它只让 CPU 翻译和 GPU 执行重叠，Vulkan 翻译仍同步，尚未接入 Foundation JobSystem。

119 的基础预算为 Draw 自身 21.854 ms、范围预检 4.668 ms、GuestCondition 20.677 ms、未标注 7.960 ms。独立 HLE 窗口发现 WindowAcquire 等待 renderer 锁；BuilderSetDefaults/Swizzle 还错误地使用全局翻译锁。Builder 已完全存储在调用者自己的 NVN 对象内，120 按 Mac 实现移除此锁。HLE 窗口有丢块，累计时间只用于定位，不当作完整帧或性能收益。120 再采显示阻塞主要转移到 MemoryPoolFlushMappedRange。

### 同窗调度验证

119 的 candidate119-joint 保留了原始 PROF、16 秒 sched Perfetto、前后 CNTVCT/BOOTTIME 锚、进程/boot 身份和 GPU busy 样本。通过 Litep SQL 取得 SDK 帧 102–199 与主 NVN 线程 22590 的区间，再将已解码区间按时钟锚映射到原始 Perfetto。没有私自解码 PROF，也未直接读取其 SQLite。

98 个完整成功呈现帧共 5932.479 ms。NVN.Other 翻译阶段经过 3062.939 ms，其中实际 Running 3031.402 ms（30.933 ms/帧），runnable 约 31.537 ms；GuestCondition 经过 2069.028 ms，其中睡眠 2060.781 ms。其它区间 800.513 ms 中仍有 766.835 ms 睡眠，不能把未标注时间解释为 CPU 执行。两个时钟锚计算出的偏移漂移约 58 ns，保存的 phase-scheduler.sql/csv 和 scheduler-report.json 可复核。

同窗设备级 gpubusy 加权约 23.86%，GPU 频率样本为 660 MHz；它包含系统 GPU 工作，不等同于 BOTW 专属利用率。PROF 文件采集线程本身用了约 3274.8 ms CPU，必须从游戏线程瓶颈判断中排除。翻译阶段主要在执行 CPU 工作，但全帧仍有等待，因此尚不能宣称整个帧循环已经 CPU 饱和。

Odin3 不匹配现有 AYN Thor root adapter，未绕过适配器提权。本轮使用普通 ADB 可读的 Perfetto 调度与 KGSL busy 数据；没有 root tracefs KGSL sidecar，不能宣称已获取 GPU submit/retire 的完整调度证据。

### 回归与证据

强制阈值 1 的 probe118/probe120 在跨提交槽的 GPU 链上验证 576 个 storage words：独立目标、完全覆盖 GPU view、部分覆盖 CPU 合并、旧完整写入被新部分写入覆盖，以及中间独立清屏和空 Dispatch。全部通过，并验证实际触发至少 8 次中途提交。仍通过百万对象 churn、66000 次录制回收、2048 条纹和复制纹理保留 GPU 内容等原有检查。probe120 新增持有翻译锁时 Builder 完成的检查。合成像素回读不能替代长时间实际游戏画面验收。

原始目录为本机 Git 外 porting-20261024-lifetime：

- 119 APK：f0c625ce27db2aae763388c7337f5b40a46cf26366ae4ecb4b64094cf05090d2；PID22308；基础 PROF：7d23830c573c557e03d1bd5ccc7ef06fb4494f0a4d07a6600158d50661dc036d。
- 120 APK：fb34ceb7a60a22980f1734d7d0def0f394a6591bd8bdc4bff052d0e0a60911e3；PID25158；基础 PROF：dc8968fd1563e5c1b7237cc90d22d070d57eef73a08d7b7ce6b2456379d36070。
- 119/120 HLE PROF：fc23dfd5b6d7c1adf60afaca886a4107f281c50dc109c716e7cac2509d51013d、c20646e6e699d238ce985d39a767f53cfa6bbe2a49a027e15a0e96b947e61004；均保留掉块诊断。


## 2026-10-25：Mac 双后端、动态 pipeline 状态与 LRU

共同 Vulkan 路径现可在 Mac 的 MoltenVK 下运行，原生 Metal 目标复用 MacNative。使用本机 avp_study 的 Instruments 录制和 native reader 获得两条路径的 CPU / Metal 证据；没有用截图推断性能。身份、测试与捕获限制见 [Mac 验证记录](../macos/VALIDATION.md)。

本轮确认 Vulkan 的 depth bias、blend 常量和线宽进入静态 pipeline key，fast/memo 命中又没有刷新 LRU。实际运行到 4,096 对象上限后，一段 60 帧窗口产生约 10,108 次重建/淘汰。现按 Metal encoder 边界使用 Vulkan core dynamic state，逐 draw 设置实际值；所有缓存命中都刷新使用时间。旧 warm list 保留，新列表按 key 策略隔离；driver cache 继续复用。

修复后的已观察窗口约 1,300 个 pipeline、零淘汰，部分窗口无新建；4 对象压力回归证明热对象不会被 fast path 误淘汰，16,384 像素验证动态 blend 与 pipeline 复用。不同时间段场景变化，不能将 31–43 次呈现/秒写成严格 A/B 提升率。Android 只完成交叉构建，仍需实机同场景 Litep 验证。

Mac 还暴露并修复了 BC heap alias、3D BC 切片、重复 G-buffer 附件、自定义边框与大于池的 buffer cache range，并补接命令式 Texture/Buffer 复制。Vulkan 翻译仍同步，Present 仍有 engine 锁和 QueueWaitIdle，Foundation JobSystem 尚未接入。CPU/GPU 饱和目标仍未达成。

## 2026-10-25：呈现复制等待释放 engine 锁

Mac 与 Android 共用的呈现路径现在按准备、完成、退休分段。准备在 engine 锁内提交游戏内容和呈现复制，完成阶段只等待这次复制的独立 fence，退休重新持锁。等待期间允许下一批同步翻译执行；源 Image 及任务闭包保留到真实 GPU 完成。呈现前的范围预检保留相交读取同步，不再统一兑现全部无关 GPU 写回。交换链 semaphore 按 image 持有，避免把复制 fence 当成 WSI 已消费 semaphore 的证明。

`SerialHandoff` 的有界准入、提交顺序和 image ticket 契约不变。共享 GPU 探针新增阻塞完成回调、并发翻译/源纹理 Finalize 与 2,048 像素读回，完整 356 次提交和 12 项底座测试通过。CPU/GPU 统计在解锁前快照，完成线程不访问 renderer 非原子计数。三段 Litep scopes 用于后续验证等待是否减少，而不是仅改变所在的线程。

本轮较重场景的旧进程与新版菜单工作量不一致，不做 FPS A/B 收益结论；Foundation JobSystem 与提交输入完整快照仍未接入。身份、原始记录和后续实测见 [Mac 验证](../macos/VALIDATION.md)。
