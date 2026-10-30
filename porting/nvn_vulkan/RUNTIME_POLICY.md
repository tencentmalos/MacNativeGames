# Mac / Android 共用 Vulkan 策略

## 实现与边界

两端链接同一个 `mng_nvn_vulkan` 和同一个经过适配的 `dm_vulkan_engine`。
`prepare_engine.py` 只从锁定的只读 D2R 源码生成构建目录副本，Mac / Android
不各自保存渲染器分支。NVN 录制、不可变命令、同步翻译、RenderPass、读写依赖、
纹理存储和回收均在本目录修复，两端重建后共同验证。

`runtime_policy.h` 是宿主无关默认值的唯一入口。引擎初始化首先验证并补全策略；
独立 GPU probe 同样经过此入口，不能依赖 Mac 启动脚本才能开启优化。
Mac `run.py`、`host_diagnostics.cpp` 和 Android JNI 不再各自设置 Vulkan 默认值。

| 层次 | 负责内容 |
| --- | --- |
| 共享 Vulkan 策略 | 附件反馈快照、片元屏障合并、管线缓存、命令分批、呈现交接、范围依赖检查及性能计时策略 |
| 共享实现 | NVN→Vulkan 翻译、资源所有权、Fence、CPU/GPU 读写依赖、呈现源图像生命周期、Litep 与 debugbus |
| 平台适配 | MoltenVK / Turnip 装载、窗口 Surface、显示节拍输入、宿主文件路径、平台显式设置传输 |
| 逐游戏适配 | Build ID 校验、Guest Patch、BOTW 实时时钟与 60 FPS 补丁，不作为 Vulkan 通用开关 |

Metal 后端继续复用 MacNative 的实现；这里统一的是 Mac MoltenVK 与 Android Vulkan。
能力差异仍由真实设备 feature / limit 决定，例如不支持 depth bounds 的设备不能假装支持。
着色器归档路径与 Android SAF 导入属于资源部署；当前两端仍使用同一个着色器读取实现。

## 生效值与覆盖

共同默认开启反馈快照、片元依赖合并、管线 memo、呈现交接、范围预检查、范围 Flush、
读范围缓存、buffer 过滤及自适应分批，批次上限为 1024；旧高频 CPU 计时默认关闭。
精确字段和取值范围以 `runtime_policy.h` 为准。

Mac 可在启动前显式设置表中的环境变量。Android Debug APK 将
`debug.mng.<key>` 转为同一个环境变量，遍历同一张表，不另列默认值。
未设置的值由共享引擎补全，非法值拒绝启动。设备属性是持久到重启或清除的诊断覆盖，
验证时必须记录，不能误当作默认。已有 D2R 显式 marker 文件仍可覆盖对应引擎选项，
因此以运行状态为证，不能只根据启动参数判断。

`renderer_schedule status` 在引擎锁边界读取实际生效值，并报告
`policy=shared_nvn_vulkan_v1`、`feedback_snapshot`、`defer_fragment_barrier`、
`pipeline_memo`、`perf_sample`、`read_preflight`、`range_preflight` 等。
运行时的 `adaptive / batch / present / invalidate / reads / descriptors / images / pipeline` A/B 控制也保持共用。
请求只尝试获取锁；引擎尚未初始化或正忙时返回可重试状态，不阻塞 Android 主线程。

`live_range_flush` / `MNG_LIVE_RANGE_FLUSH=1` 同样由共享表管理。
普通池且同步翻译时，Flush 根据当前图像、写回、report 和 event 输出租约判断交集；
无交集时绕过前端全局锁；仅图像交集由下述 `image_flush` 策略控制，其他交集保守处理。
物理／虚拟别名、未知范围以及有序翻译保持旧路径。它不替代 GPU fence、CPU 写回或纹理内容所有权。
`renderer_schedule flush on|off` 可作同进程对照，状态返回 `flush_live_fast`、
`flush_live_conflict`、`flush_legacy`、`flush_unknown` 及活动范围数量。
Odin3 的锁等待变化与帧率限制见 [活动范围 Flush 实测](ODIN3_LIVE_FLUSH_20261027.md)。

## 2026-10-26 发现的偏差

Android 首次复测进入复苏神庙后，日志约 21–22 FPS、每帧约 465 个 RenderPass。
原因之一是反馈快照和屏障合并仅由 Mac 启动脚本打开，Android 虽然链接了相同的新代码，
仍走关闭状态。首次 APK 为 `ab4c9cef974ddde4…`，没有完成完整的 30 秒测量，
不能把它包装为严格的同进程 A/B 基线。

该进程还因 `Image::shared_from_this` 的 `bad_weak_ptr` 退出：对象回收中获取命令槽，
可能解决待写回数据并重建图像索引；随后删除存储记录却没有再次使索引失效。
修复在每次删除后重新标记索引失效，保持原有 Fence 延迟销毁，不跳过 GPU 等待。
回归测试直接调用生产实现并模拟退役过程中的写回索引重建，旧逻辑失败、修复后通过。

策略与生命周期回归在宿主和 Android 通过；相同策略下 GPU probe 在 Adreno 830
与 Apple M4 Pro / MoltenVK 均通过。场景帧率以实机记录为准，不能从 probe 推导。
本轮构建、安装身份和采样保存在 Git 外的
`native-switch-study/android-device-20261026/`，游戏资产和存档未纳入仓库。

## 内存与翻译检查

`descriptor_plan`、`image_read_fast`、`pipeline_early`、`upload_cached` 也由同一张策略表管理，
并在 `renderer_schedule status` 返回。前三项可在引擎边界进行同进程 A/B；
`upload_cached` 影响 arena 分配，必须重启，再核验实际 memory type / flags。
它只影响 CPU 填充的上传 arena，不改变 Guest 数据快照、GPU fence 或读回策略。

`profiler_detail start 32` 每 32 次 Draw 选择完整的一次内部细节；省略步长仍为全部。
`NVN.Draw.Encode` 分离命令编码，`NVN.Translate.ElapsedNs` 和
`NVN.Translate.ThreadCpuNs` 分离批次经过时间与本线程实际 CPU 时间；
`ThreadCpuValid=0` 表示线程时钟不可用。两次线程时钟读取发生于每批翻译边界，
不在每个 Draw 中读取。批次经过时间扣除线程 CPU 时间包含等待和被抢占，
不能仅由这个差值确定等待原因，也不包含驱动其他线程的 CPU 时间。

本轮测量口径、实机 A/B、UMA 差异和并行化边界见
[Android 翻译分析](ANDROID_TRANSLATION_20261026.md)。

PSO 组合缓存及提前命中的边界见 [NVN 管线模型](NVN_PIPELINE_MODEL.md)。

## PSO 准备任务

`pso_workers` / `MNG_PSO_WORKERS` 是共享的启动选项：0 自动选择，1..8 显式指定。
当前自动值为 `min(4, max(1, hardware_concurrency / 2))`；前台可以领取未开始的任务，
因此峰值执行数最多是 worker 数加一个有序消费者。`renderer_schedule status` 返回
实际 worker 数、submitted、built、failed、active、peak、joined、claimed。

`profile_startup` / `MNG_PROFILE_STARTUP=1` 显式开启首帧前 Litep 文件采集，默认关闭。
使用 SDK 冷控制面的 mode transition，不虚构帧或给初始化线程冒领呈现线程身份。
采集上限 64 MiB / 60 秒，完成后恢复关闭的 ring；脚本通常在预热完成后提前停止。
旧 D2R 的跳 Draw 异步编译在本移植中关闭，已知任务在有序消费者处领取，失败／未知 key
正常回退前台构建。详细测试和数据见 [PSO 预热](PSO_WARMUP_20261026.md)。


## 仅图像范围的 Flush 发布

共享策略 `image_flush` / `MNG_IMAGE_FLUSH` 默认 1，Android 诊断属性为
`debug.mng.image_flush`。普通池、活动范围检查开启且同步翻译时，仅与独立 Vulkan
图像存储相交的 Flush 执行 CPU 发布屏障，不再等待翻译全局锁。任何写回、report、event
或未知所有权混入时仍保守等待。Invalidate、GPU fence、读回和资源退休保持原行为。
`renderer_flush_image status|on|off` 支持同进程对照，`flush_image_fast` 记录累计命中数。
Mac/Android 使用同一份实现；不是只在 Android 放宽依赖。

Odin3 四轮实测汇总 20.91 → 21.28 FPS，约 +1.7%；Flush 等待消失后，主线程纹理
初始化和 Guest WorkerMgr 同步成为更明显的串行边界，不能宣称已经解决低帧率。
完整证据与下一步见 [图像 Flush 对照](ODIN3_IMAGE_FLUSH_20261027.md)。

## W0 发布解耦诊断（默认关闭）

共享策略 `frontlock_diagnostics` / `MNG_FRONTLOCK_DIAGNOSTICS` 默认 0。
`frontlock_status on|off|status|reset` 在运行时控制固定 128 项等待表，记录等待 API、
Guest handle、等待开始时观察到的持锁 API、次数、总 ns 和最大 ns。
持锁 API 可能在等待期间变化，不能把该字段解释为精确的全程锁归属；
`unknown` 保留未经过 FrontLock 的持锁路径。溢出单独计数，reset 使旧采样令牌失效。
控制面仅尝试取得统计锁，繁忙时返回可重试错误，不等待渲染器锁。

`renderer_ordered_inputs status` 只读返回有序翻译 reason 5 的细分计数和首次拒绝现场。
此诊断不扩展 opcode、输入范围预算或有序模式默认值。
`Native.PublicationDeltaValid` 表示当前 Present 的增量基线是否可用；
有效时输出 `ImagesEpochDelta`、`ImageIndexRebuildsDelta`、
`TextureInitializationsDelta`、`TextureFinalizationsDelta`，均为 `Native.` 前缀。
增量对应宿主 Present 边界，不保证等于 Guest 逻辑帧。

验收和阶段停止条件见 [发布解耦任务书](SPEC_PUBLICATION_DECOUPLING_20261027.md)。

## W1 有界发布队列（默认关闭）

`publication_queue` / `MNG_PUBLICATION_QUEUE` 默认 0，容量为共享策略中的 4096 项。
`renderer_publication status|on|off` 控制运行中的共享 Vulkan 前端；有序翻译开启时不允许启用。
队列按值保存 RetireTexture、RegisterTexture、RegisterSampler，Guest 侧不进入引擎。
每次最外层 FrontLock 获取后和 Submit 前排空；关闭时先停止新入队再排空。
pool/descriptor pool 的重建或释放仍通过原 FrontLock 边界；shutdown 单独关闭 admission 并排空。
队列满时走原同步路径，保留之前的发布顺序。drain 失败保留未完成批次并明确 fatal，不静默丢失。

状态包含 enqueued、drained、overflow、max_depth、pending、in_flight、drain_ns_total/max/calls，
以及 retired / registered_textures / registered_samplers。后面三者之和应与 drained 对账。
retired 是入队操作数：既含显式 Finalize，也含未 Finalize 的重新 Initialize；按值副本可能再次
发布同一 serial，沿用引擎原有幂等退休语义，不增加永久去重表。故不能简单把 retired 与
texture_finalizations 总值相等当作正确性条件。

## 2026-10-27 W1 实机验收结果

发布队列两轮验证后，最终同进程 ABAB 为 21.129 → 27.993 FPS（+32.49%）。
尚未满足 W0 基线的 P 锁等待门槛，唯一 identity 退休对账也未闭合；按任务书两轮上限停止。
`publication_queue` 默认及最终运行状态仍为 0；TextureCompare 去锁同样受此开关控制。
Mac/Android Vulkan 共用实现；构建、lint、26 项 CTest 与两端三模式 probe 通过。
详细报告：`porting/nvn_vulkan/ODIN3_PUBLICATION_W1_20261027.md`（相对仓库根目录）。

## 2026-10-27 Thor 基线停止记录

按用户要求改用 Thor `9c2841a4` 后，publication=0 / ordered=0 的安装前 probe
触发 GPU report 与 Guest 时钟对齐检查失败，返回 1。日志中换算时间落后于宿主观察时间
约 1030 ms；具体计时源或查询生命周期原因尚未证明。
按任务书停止条件，未安装新 APK、未进行性能 ABAB，默认开关保持 0。
没有新增设备专属渲染路径。详见 [Thor 验证报告](THOR_PUBLICATION_20261027.md)。

## 2026-10-28 Odin3 复测

复用 W1 第二轮 APK，Odin3 三模式 GPU probe 通过。同进程四段对照为
21.338 → 27.977 FPS（+31.11%），剩余 Invalidate 锁热点再次出现。
未修改渲染器或默认值；W1 严格验收和 identity/lifecycle 证据缺口仍未关闭。
测试结束恢复 publication=0、采样关闭、启动属性原值，游戏进程保留。
详情见 [Odin3 复测](ODIN3_PUBLICATION_RECHECK_20261028.md)。

## W2：按输出依赖决定 Invalidate 是否等待

`invalidate_fast` / `MNG_INVALIDATE_FAST` 默认 **0**；Mac MoltenVK 与 Android
共用实现。Android 仅把 `debug.mng.invalidate_fast` 传入共享策略。
`renderer_invalidate status|on|off` 可热切换，命令只操作原子状态，不等待渲染锁。
`invalidate_fast` 是请求值，`invalidate_effective` 还要求同步翻译及范围 Invalidate 开启。

普通池先查询独立的活动范围登记表。没有 CPU 输出交集时只执行 acquire fence；
图像权威状态单独存在于 Vulkan image 中，不代表 Guest CPU 存储需要回写。
Writeback、Report、Event、Unknown 或将来新增的未知类型有交集，仍进入原来的
前端锁和范围写回路径。物理／虚拟别名、有序翻译、`renderer_schedule invalidate off`
保持保守路径。不改 Flush、不取消 GPU fence，也不改变引用回收。

状态包含 `invalidate_clear/conflict/legacy/unknown` 累计次数；Litep 包含
`NVN.Invalidate.Fast/Resolve` 和 `LiveKinds`。`benchmark_publication.py --mode invalidate`
要求 `publication_queue=1` 固定，以 OFF/ON/OFF/ON 单独测量 Invalidate 收益。
进程 CPU 占比按 `/proc` 的进程 ticks / 全 CPU ticks 计算；KGSL `gpubusy` 仅保留
驱动最近区间的原始值，不冒充与 30 秒 FPS 或 Litep 对齐的 GPU 利用率。

## 2026-10-28：Profiler O2 与宿主 Hint 实验

共享 profiler 集成在 Debug 下为 Foundation ring、SDK、LZ4追加O2，保留调试信息和断言；
没有修改 Foundation 源码。低扰动门槛通过，详见[O2复测](ODIN3_PROFILER_O2_20261028.md)。

`renderer_hint on|off|status` 是默认OFF、进程局部的S5实验。公共队列与Vulkan钩子共用，
Android适配器动态解析NDK API，Mac明确unsupported。独立worker独占后端，两个通道
各保留一个待处理样本，不在renderer锁内调用Binder；generation/applied证明开关已生效，
TID/accepted/reported/coalesced/failures说明实际工作与失败。Session仅绑定本进程实际
工作TID，失效后不逐帧重试，OFF和停机关闭；不改变Guest调度或渲染依赖。
本轮+1.942%未达+2%，五分钟窗口中断、频率/p99对照缺失，保持OFF，
不能标记S5完成。见[设计](ODIN3_ADPF_DESIGN_20261028.md)与[结果](ODIN3_ADPF_20261028.md)。
