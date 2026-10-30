# BOTW Android：参考社区实现的有序 Vulkan 翻译

日期：2026-10-27。

本文保留初版实现与验证结果。后续已增加 [counter report 的受约束输出支持](COUNTER_REPORT_20261027.md)；[最新实机报告](ODIN3_COUNTER_REPORT_20261027.md) 中整批提交仍在 Draw 输入检查处同步回退，有序模式继续默认关闭。后续本项目 Odin3 实机验证已获持续授权，替代本文末尾初轮逐包授权要求；备份和数据安全边界不变。

## 结论

本轮已实现并验证**默认关闭、可对照的有序翻译通道**。通过输入所有权检查的提交，由单独的 `mng-translate` 线程执行；执行期间不持有 `front_mutex`。不支持的提交保持原有同步路径，先排空此前工作，不跳 Draw，也不放松 GPU 依赖。

这不是“所有全局锁已删除”：提交快照、准入和必须同步的资源变更仍串行；Vulkan 引擎锁及同一队列的外部同步继续保留。

**后续实机结果（2026-10-27）：未通过性能验收。** Odin3 上串行加权 21.154 FPS，有序 19.444 FPS；两个有序窗口的 Translate 全部因未支持 opcode 同步回退。下面的合成测试覆盖率不代表 BOTW 覆盖率，不能宣称达到社区的 30 FPS。完整对照及保留审计见 [Odin3 有序翻译实机验证](ODIN3_ORDERED_AB_20261027.md)。

## 参考与范围

- 只读参考：Citron 的 `references/native/BOTWRecompAndroid`。
- 仓库：<https://github.com/mamcjr/BOTWRecompAndroid>。
- 固定提交：`ef7a3062432f33457155020b40595b81c9827a41`，标签 `v0.1.11-beta`。
- 重点入口：[async.c](https://github.com/mamcjr/BOTWRecompAndroid/blob/ef7a3062432f33457155020b40595b81c9827a41/runtime/nvn/vulkan/async.c)、`runtime/nvn/objects.c`、`runtime/nvn/vulkan/exec.c`。
- 借鉴有序请求、翻译完成序号、背压及资源排空的设计，重新实现适合当前 C++ 适配层的所有权和同步协议；没有链接该参考仓库，也没有改写其文件。
- 没有照搬其 `QueueWaitSync` 成功桩或忽略调用者 timeout 的 `SyncWait` 行为。
- 参考项目从 LAS Recomp 派生，CPU 路径是 AArch64→C 静态重编译；本工程仍是直接 AArch64 执行。社区 FPS 不是同设备、同场景基线。
- 修改对象是 **MacNativeGames 的 NVN→Vulkan 适配层**，不是 Citron 自身的 NVN frontend。固定 D2R 源码保持只读；通过 `prepare_engine.py` 生成构建目录中的适配版本。

## 实现

### 1. 单消费者和有界准入

`porting/runtime/ordered_executor.h` 提供一个渲染所有权线程，不替代 Foundation 的 PSO 工作池。

- 最多 64 个已接收请求，正在执行的请求也计入。
- 已接收请求的申报输入载荷预算为 64 MiB；单个提交的 Guest 字节快照最多 32 MiB。
- 命令数组及 `CommandCopy` 保活，提交后重录或回收公开句柄不改变正在执行的命令。
- 发布顺序、CPU 完成序号和结果保存在 Host ticket 中；worker 不写 Guest sync 指针。
- 输入释放后才发布 CPU 完成；异常保存在 ticket 中，后续请求拒绝或携带同一失败完成。
- `close()` 停止准入并 join 已接收工作。**这里有界的是请求数和载荷，不是驱动调用的墙钟上限**：若底层 Vulkan 永久不返回，不能安全强杀线程再销毁资源，本轮未实现驱动挂死的硬超时恢复。
- 64 MiB 不是整个进程 RSS 上限；调用者正在准备的一个快照、既有命令注册表、固定队列元数据、线程栈及 GPU 资源另计。

### 2. 提交时输入快照

`submission_inputs.h` 按原地址合并范围，复制后保留原地址→快照的映射，**不改写 GPU 地址**。`ordered_submission_engine.inc` 从继承绑定状态的副本开始，扫描本次命令、解析实际 shader variant，并收集：

- 当前及继承的 UBO；
- 只读 storage buffer；
- 实际使用的顶点流、索引；
- 采样纹理、只读 texel buffer，以及附件可能发生的 CPU 初始上传；
- UI 覆盖判断所需的小范围 CPU 读取。

工作线程的 uniform/general snapshot、8 位索引展开、纹理 hash/deswizzle/线性复制和 UI 读取改为访问这些不可变字节。仍以原地址做别名和 GPU 依赖判断。异步路径不使用可能重新读取活跃 Guest 页的顶点/storage/texel CPU 缓存快路径。

shader family 在快照阶段解析；program 快照为会话持有的不可变对象。描述符注册复制元数据并进入同一有序队列；pool 的销毁、重映射与 program 的释放在禁止新 publication 的临界区内排空此前工作，并保留原 GPU 退休要求。

### 3. 明确回退，不猜输入语义

以下情况当前走同步路径：

- CPU 可见写入、拷贝、事件、计数器报告、间接参数及其他未列入允许集合的命令；
- shader 可写 storage/image、buffer 输入与本提交附件写入重叠；
- 待退休 GPU writeback/report/event 可能改变捕获范围；
- 无法解析的绑定、非法或超预算范围；
- 已启用的部分捕获/纹理诊断模式。

新 opcode 默认不支持异步，不以黑名单遗漏新指令。同步回退仍执行完整命令，不忽略等待，也不把旧 CPU 字节冒充 GPU 结果。

这组规则对真实 BOTW 可能仍较保守，且完整纹理快照有复制成本。合成测试的进入比例不能推算游戏中的进入比例或收益。性能测量和诊断捕获应分开运行；切换捕获模式前应回到串行配置，不能用正在执行的异步数据去拼接未经验证的捕获证据。

### 4. 锁和资源变更

- 普通提交先等待前一批翻译完成以确定继承状态，再捕获数据、发布请求并返回；**不是多个 Translate 并行执行**。
- worker 不获取 `front_mutex`。命令录制继续使用独立的 `command_mutex`。
- 纹理/采样器注册和纹理释放成为复制参数的有序消息。普通 `TextureFinalize` 没有新增全队列 idle。
- Proc 查询、标签和窗口值型更新不再默认排空 Translate。
- 读回、pool 销毁/重映射等操作仍有显式同步边界。排空期间同时排斥新发布，避免“检查到空队列后，另一生产者又入队”的竞态。
- `FrontLock` 在可能阻塞前归还 Guest 虚拟核心，释放 Host 锁后才重新取得；原有 `HostWait` 的嵌套调用由已有 core-away 返回值保持平衡。
- 下一次提交及 Present 的保守 capture/drain 仍会持前端锁等待，尚不能把所有前端等待都描述为消失。

### 5. Fence 与 Present

- QueueFenceSync 的 Host ticket 先等待有序翻译点，再解析为原引擎 GPU fence。
- SyncWait 的零超时是 poll；有限 timeout 在“翻译等待 + GPU 等待”两阶段共用一个剩余预算。
- SyncFinalize/reinitialize 删除旧 Host 映射；老 worker 不会向新 Guest sync 写回完成。
- QueueWaitSync 保留真实 GPU 依赖，Flush/Finish 的区别不变。
- Present 保留 Guest 侧步长/节奏更新。捕获 CPU-backed 呈现输入前保守排空此前翻译，再发布有序 Present。
- 现有 presenter 仍单独执行 GPU 完成等待。快照会保留到 presenter 的 CPU preparation 结束，而不是只等 presenter 取得引擎锁。
- 每窗口最多保留一个尚未解决的 Present 请求；Acquire 使用对应 image 的翻译/呈现 ticket，不能用“最新帧”或仅准入序号替代。

## 开关与观测

共享启动策略：`MNG_ORDERED_TRANSLATION=0|1`，默认 `0`。Android Debug 构建只负责把 `debug.mng.ordered_translation` 传入同一策略，不另设默认，也没有运行中热切换接口。

`renderer_schedule status` 增加：

- `ordered_submits`、`ordered_fallbacks`、最近原因及 `ordered_fallback_1..7`；
- `ordered_capture_bytes`、`ordered_drains`；
- `ordered_admitted`、`ordered_completed`、`ordered_pending`；
- `ordered_bytes`、`ordered_peak_bytes`。

原因：1=诊断/模式不兼容；2=附件输入范围；3=命令结构；4=未支持 opcode；5=绑定、输入预算或写入/别名所有权；6=池范围或待退休写入；7=组合载荷预算。

`admitted/completed` 包含注册/释放等请求，**不是帧数，也不是 GPU 完成数**。状态读取使用 try-lock；忙时返回 busy，不阻塞 Android service 主线程。

新增 profile scope/counter：`NVN.Ordered.Capture`、`NVN.Ordered.Execute`、`NVN.Ordered.Drain`、`NVN.SyncWait.Translation`、`NVN.Ordered.ExecuteUs` 和 `NVN.Ordered.ThreadCpuUs`。CPU 时间读取失败时不报告为零执行。已有 API scope 可定位排空发生在哪个入口；未增加每 Draw 的时间戳采样。

## 验证结果

### Host、工具和构建

- 8/8 相关 CTest 通过：ordered executor、runtime policy、preparation plan、conservative ranges、serial handoff、image storage、GPU clock、present pacer。
- 新 executor/input 测试通过 ASan + UBSan；覆盖 6 个生产者共 6000 次有序发布、请求/字节背压、poll/有限等待、错误传播、关闭后拒绝、ticket 生命周期和不可变范围快照。
- 26/26 Python 回归通过。
- Android `assembleBotwDebug` 和 `lintBotwDebug` 通过。
- generator 语法、增量 diff 空白检查通过；两个只读参考的工作树保持干净。
- CTest 从构建目录的 `porting/` 子目录运行；顶层 macOS 工程没有注册根测试，不能把顶层“0 tests”当通过证明。

### 真实 GPU 合成回归

主机 Apple M4 Pro；独立诊断 MoltenVK 1.4.2，未替换 Citron 或系统驱动。

驱动 SHA-256：`8ea25172b62b40919bcb60d24c7d93970ac625921a38eb625e66d57f52e8ea8e`。

| 模式 | 结果 | NVN 提交 | Draw / Dispatch | 异步 / 回退 |
|---|---|---:|---:|---:|
| 串行，准备缓存开启 | PASS | 400 | 178 / 10 | 不适用 |
| 有序，准备缓存开启 | PASS | 400 | 178 / 10 | 384 / 16 |
| 有序，准备缓存关闭 | PASS | 400 | 178 / 10 | 384 / 16 |
| 有序，legacy UBO 描述符路径 | PASS | 400 | 178 / 10 | 384 / 16 |
| 有序，显式 border fallback | PASS | 400 | 178 / 10 | 384 / 16 |

各组启用 `MTL_DEBUG_LAYER=1`、`MNG_PREPARATION_VALIDATE=1`、4-entry pipeline cache 和异步 MoltenVK queue submit。保留完整像素/word 断言，没有删测试或跳 draw。日志未发现 Metal validation error；这不等于运行了 Android Vulkan validation layer。

新增确定性检查把 worker 停在执行前，然后：

- 覆盖 UBO 并回收命令句柄，验证 16384 个动态 UBO 像素；
- 只继承上次绑定、不重新 Bind，修改同地址 UBO 后验证 8192 个像素；
- 覆盖纹理和索引，验证 8192 个采样/swizzle 像素；
- 覆盖 packed vertex 数据，验证 4096 个像素；
- 在翻译未完成时检查零/有限超时，复用 sync 后确认旧请求不能误触发；
- 呈现完成仍被 gate 阻塞时，Acquire 另一 image 能返回，Acquire 正在呈现的 image 必须等待；返回后 acquire sync 才可报告完成；
- 原先的独立翻译进展、GPU 写回链、附件别名、反馈、命令重录、资源退休、2048 个呈现复制像素等测试继续保留。

有序组结束时 `pending=0`、`bytes=0`；11 个 opcode 回退、5 个输入/写入所有权回退。完整范围快照总字节数和队列峰值保存在结果 JSON；它们只描述合成负载。

当前主机 depth-bounds 功能未覆盖（0 像素），border fallback 本来就可能必需，因此强制组不是 native-border 与 fallback 等价性的额外证明。没有启动 BOTW、没有验证 Android WSI/屏幕显示或真实游戏 shader 全集。

## APK 与证据

最终 APK：`android/app/build/outputs/apk/botw/debug/app-botw-debug.apk`。

完整身份与内容差异见 `android/build/nvn-ordered-translation/apk-verification.json`；原包保存在同目录的 `before-app-botw-debug.apk`。本地交付时校验 ZIP 条目仅 `lib/arm64-v8a/libmacnative_platform.so` 改变，Turnip、界面资源及其余条目保持相同。后续 Odin3 部署及运行结果见 [实机报告](ODIN3_ORDERED_AB_20261027.md)。

证据目录：`android/build/nvn-ordered-translation/`。

- `gpu-final2-results.json`、`final2-*.log`：最终五组 GPU 结果、选项、binary/driver SHA、Acquire 检查。
- `host-tests.log`、`sanitized.log`、`python-tests.log`：Host 回归。
- `android-delivery.log`：最终 APK/lint 构建日志。
- `baseline/`、`implementation.patch`：本轮前的文件快照，以及不包含原有未提交工作的 task-only patch。

保留的失败尝试：初次新捕获代码有一处条件数组类型编译错误；早期历史 GPU 范围判断过度保守，阻止合法的纹理存储复用为 UBO；试图省掉附件捕获时，部分 clear 的保留内容读取触发 fail-closed 检查，已恢复附件输入捕获；顶点覆写测试最初没有恢复后续旧测试需要的数据，现已恢复；首次 sanitizer 编译缺匹配 SDK，明确指定 SDK 后通过；一次 Android 构建未传完整 BOTW asset-config，补齐原有配置后通过。没有通过弱化断言解决这些问题。

## 本地交付时的实机验收边界（历史记录）

上述本地构建阶段没有安装、重启、Root、锁频、清数据、删缓存或恢复 `option.sav`。随后用户单独授权了 Odin3 实机验证，结果已补充在本文开头及实机报告中。

验收要求为：对最终 APK 单独授权后进行同场景串行/有序对照，先 profiler-off 测吞吐，再单独采连续 trace；同时检查画面、Draw/Dispatch 工作量、回退比例、快照/排空时间和输入延迟。真实测试已确认全部 Translate 回退，性能没有改善；尚未测量输入延迟，不能用本轮合成测试或社区 FPS 代替结论。
