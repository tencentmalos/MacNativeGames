# BOTW 两阶段复核：Guest 边界标记、精确回退与排空短锁

日期：2026-10-27。

## 当前结论

本轮实现了可关闭的 Guest 等待链诊断、具体 opcode 回退证据，以及一项排空期间的前端锁修正。**后续 Odin3 实机验证已完成，仍未证明 FPS 改善：串行 20.928 FPS，有序 19.406 FPS；首个回退点确认为 `OP_REPORT_COUNTER`，Guest 等待关联到 `nn::os::ReceiveMessageQueue`。** 具体通知链、准备缓存成本、约 22% 的标记扰动及数据审计见[实机报告](ODIN3_GUEST_BOUNDARIES_20261027.md)。

前两阶段的负面结果仍然成立：准备缓存没有证明收益；有序翻译在 Odin3 真实场景中全部同步回退，串行 21.154 FPS、有序 19.444 FPS。不能用本轮合成通过率改写这个结论。

## 为什么结合 Guest 标记

已有稳态 trace 中，每 source frame 的 Translate 约 27–28 ms，Draw 约 19 ms，Preflight 约 4.4–4.8 ms；同线程还有约 19–20 ms 的 GuestCondition 等待。这些是不同作用域的经过时间，不能简单相加推算帧率或 CPU 利用率。

第一阶段复用的是 shader binding 结构、range 形状、descriptor key 和固定管线状态；当前 buffer 内容、image preparation、pending writeback/report 仍需检查。高复用率没有说明这些消费路径便宜。第二阶段在整批回退时保留了资源消息与 Present 的额外工作，却未把主 Translate 搬到 worker。

因此需要同时回答：

- 哪条命令最先使真实提交回退，位于哪个命令句柄和字节位置？
- Guest 的等待在哪里发生，真正选中该等待者的通知来自哪个线程、哪个 Guest 边界？
- 通知之后是否还在等 mutex、Host 调度或虚拟核心？
- 第一阶段命中之后，Draw 的实际成本还集中在哪个准备步骤？

## 1. 具体 opcode 回退证据

`ordered_submission_engine.inc` 输出第一阻断指令的 opcode、提交数组中的句柄序号，以及该命令缓冲内的字节偏移。它们不含 Guest 指针。

`renderer_schedule status` 新增：

- `ordered_first_opcode/handle/offset`：最近一次原因 4 提交的第一阻断位置；并非整个进程最早的失败，也不一定属于最近一次其他类别的回退。没有该证据时 opcode/handle 为 `UINT32_MAX`。
- `ordered_opcode_<n>`：该 opcode 作为第一阻断点的提交次数，不是整批中该指令出现的总次数。
- 固定 128 个计数桶，越界计入 `ordered_opcode_overflow`；状态最多输出 8 个非零桶，并报告 `ordered_opcode_rows_omitted`。
- PROF 中的 `NVN.Ordered.FallbackOpcode/Handle/Offset`：与同线程本次 Capture/回退相联系。

没有放宽任何 opcode 或输入所有权检查。合成回归观察到 opcode 5/31/34/38（counter report、buffer-to-texture、texture-to-buffer、texture clear）共 11 次原因 4 回退；**这些不是 BOTW 实机的阻断点证明**。

## 2. 复用已验证 Guest 重写门，不盲目写新补丁

新命令：

```text
profiler_guest status
profiler_guest start
profiler_guest stop
```

默认关闭；开启但未录制时不产生边界事件。开关复用现有 DebugCommandRegistry 与精确 PID 的 profiler 传输，没有新增服务。

### 原始代码位置

Android loader 原本就校验整个模块清单、源身份与重写镜像，且每个 SVC gate 具有原始 `module_offset`。本轮利用这条既有 Guest patch 映射，在 wait/signal/sleep 等 SVC 入口记录 `Guest.PatchedSVC`，包含：

- SVC 编号；
- `Guest.Site.Module` 与 `Guest.Site.Offset`；模块索引在 `guest_modules` 的 `module_index` 中明确给出；
- 最多四个可读 AArch64 FP frame record 的返回地址，转换成模块与偏移；无法映射者为 module=-1，不记录裸代码地址，不使用最近符号猜名字。

gate 保存的 x30 是门自身的 trampoline 返回点，**不是原 Guest 调用者**，因此没有把它冒充调用链。FP 记录是有界调用线索，不是完整、经过静态验证的 unwind。必须绑定本次 PID、运行代次、模块 Build ID 与清单 SHA；符号解析优先使用精确清单中的 vendor function 范围。

这不是 Citron 的 Dynarmic auto-tag，也没有在直接执行后端添加翻译探针。没有新增猜测地址的跳转、私有 SVC 或游戏逻辑替换；原 realtime/60 FPS patch 未改。

### 条件变量的真实因果边

`games/totk/runtime/threads.c` 在实际等待队列与选中通知处分开记录：

| 事件 | 证据 |
|---|---|
| `Guest.Condition.Enqueue` | 已登记的等待序号和逻辑 Guest 线程句柄 |
| `Guest.Condition.SelectedNotify` | `pick(2,key)` 真正选中的等待者，以及发出 SVC 的逻辑线程句柄 |
| `Guest.Condition.Wake` | 实际 `wake()` 发出 Host 条件唤醒的位置；可能晚于通知选择 |
| `Guest.Condition.Resume` | `block()` 返回、包含虚拟核心重新获取后的结果；超时保留真实结果码 |

等待序号不会随着转入 mutex 队列而更换。通过 epoch、wait ID、waiter 以及录制代次关联，不按相邻时间或同名作用域猜通知者。标记开关的新 epoch、录制代次变化会拒绝旧配对；零超时可以有登记/超时返回但没有通知，不伪造唤醒。

这些是本工程的 scope+counter 证据，**不是 Litep 原生 condition dependency 数据集**，不能直接宣称 `analyze_wait_chains` 已支持它。分析时还要检查丢块、缺失端点、重复通知和录制边界，保留未知部分。

## 3. 排空等待不再一直占着前端锁

最外层 `FrontLock` 的流程调整为：在锁内取得稳定的队列末尾 ticket；若未完成，释放前端锁等待；醒来重新取得锁并重新检查当前末尾；确认没有新工作后，才执行受保护的同步修改。

- 等待期间，值查询和可发布的有序请求不再被这个等待者持锁阻塞。
- 不能只等旧 ticket 就继续：其他生产者可能已追加新工作。
- ticket 自身持有完成状态，执行器销毁不会使等待引用悬空。
- renderer 所有权代次改变时拒绝继续，避免排空后落入另一代 renderer。
- Guest 虚拟核心仍在 Host 阻塞前归还，前端锁释放后才重新取得；异常路径同样释放锁。

新增 `ordered_waiters` 表示当前锁外等待旧翻译的调用者数量。

**边界没有消失：**原因 4 的同步 Translate 仍持前端锁；提交输入捕获、嵌套排空、Present 内显式排空和队列满时的发布背压不是本次全部改造的对象。这不是“全局锁已完全移除”，也没有把同步提交冒充异步执行。

## 验证

- 8/8 相关 Host CTest 通过；共享 profiler 控制测试另外通过，验证默认关闭、重复 start、epoch 递增、非法参数和未录制状态。
- 27/27 porting Python 测试通过，包括新增 `profiler_guest` 的精确 PID 传输。
- 原 scheduler/performance 两项测试通过。
- 新增生产 `svc_thread` 路径测试通过 ASan/UBSan：真实条件等待、选中通知、wake、resume、零超时和关闭标记。测试没有执行 Guest 指令，未使用的 Guest 入口会 abort 而非返回成功。
- executor/ticket/输入快照测试通过 ASan/UBSan。
- Apple M4 Pro / 原诊断 MoltenVK 上，串行、ordered+准备缓存、ordered−准备缓存三组真实 GPU 合成回归通过；每组 400 submissions、178 draws、10 dispatches。保持像素、writeback、别名、sync、逐 image Acquire 回归。
- 新锁测试主动阻塞 worker，证明另一个排空者等待时状态查询仍能完成；期间追加第二个 gated request，确认排空者必须等第二个任务结束。不是依赖随机碰巧并发的性能测试。
- `assembleBotwDebug` 与 `lintBotwDebug` 通过。

主机 GPU 测试不等于 Android Vulkan validation、不等于 BOTW 全场景验证；本轮没有实机性能数字。

## 交付和下一轮实机范围

APK：`android/app/build/outputs/apk/botw/debug/app-botw-debug.apk`。

SHA-256：`dd7dba23362eea7d58db4c75627927038004d187da3e0e47e040a309f1a4747b`。

Turnip SHA 保持 `e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`。

本轮证据：`android/build/nvn-guest-boundaries-20261027/`，含修改前逐文件备份、仅本轮差异、源 SHA、构建日志、三组 GPU 结果和 APK 身份。未提交或推送。

该 APK 已在 Odin3 `6edb0593` 完成原位部署、同存档串行/ordered 对照、Guest 标记 OFF/ON/OFF 以及每 32 Draw 详细采样。完整结果见[实机报告](ODIN3_GUEST_BOUNDARIES_20261027.md)。用户于 2026-10-27 明确授权后续本项目的 Odin3 实机验证直接执行，不再逐包确认；每轮仍核验身份、备份数据并恢复临时开关，该授权不扩展到 Root、清数据、恢复存档或更换设备。

不 Root、锁频、清数据、删缓存、改用 Thor 或擅自恢复 `option.sav`。ordered 和 Guest 标记的默认值继续关闭。只有真实回退原因、等待链和无插桩 FPS 能决定下一项优化，不能由本地通过率推定提速。
