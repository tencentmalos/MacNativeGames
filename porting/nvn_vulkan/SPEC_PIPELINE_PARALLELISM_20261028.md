# 后续优化 Spec v3：从"去锁"转向"流水线与并行"

日期：2026-10-28。分支：`codex/android-zelda`。
接续 [SPEC_PUBLICATION_DECOUPLING_20261027.md](SPEC_PUBLICATION_DECOUPLING_20261027.md)（下称 v2）。
v2 的 §3 硬约束、§6 测量规范、§7 报告模板**继续有效**，本文只写新增和变更的部分。
上游结果：[W0](ODIN3_PUBLICATION_W0_20261027.md)、[W1](ODIN3_PUBLICATION_W1_20261027.md)、
[复测](ODIN3_PUBLICATION_RECHECK_20261028.md)、[W2](ODIN3_INVALIDATE_W2_20261028.md)、
[Thor 停止记录](THOR_PUBLICATION_20261027.md)。

---

## 1. 现状

### 1.1 进展

| 阶段 | 同进程合并 FPS | 说明 |
|---|---:|---|
| W0 基线 | 21.22 | 诊断开销 < 1% |
| W1 发布队列 | 21.13 → 27.99（+32.5%） | 复测 21.34 → 27.98，可复现 |
| W2 范围 Invalidate | 28.43 → 29.39（+3.4%） | 在 W1 开启的前提下 |

两个开关 `publication_queue` 和 `invalidate_fast` 的源码默认值仍为 0。W1 的 I2（每个 identity 只退休一次）和 I5（生命周期入口 × pending 类型矩阵）证据仍有缺口。

### 1.2 资源画像（W2 ON，Odin3，同一场景）

| 指标 | 数值 | 来源 |
|---|---|---|
| 帧时间 | 中位约 33 ms，单帧范围 20–60 ms | `ab2/*.jsonl` 的 `frame_ms` |
| 进程 CPU | 约 30% 的 8 个宿主核，即约 2.4 核；每帧约 75 ms CPU | 同上 `cpu_process_ms`，以及 CPU ticks |
| GPU | 约 10.6 ms/帧（约占帧时间的 32%）；KGSL busy 约 40%（用户观察） | `gpu_ms` |
| 渲染线程 R 翻译 | 24.6 ms CPU/帧，约占一个核的 72% | `NVN.Translate.ThreadCpuNs` |
| R 等主线程 M 的消息 | 7.4 ms/帧 | `Guest.Condition.Wait` |
| M 的 `postCalc_` / `WorkerMgr::sync`（全部调用点） | 16.9 / 18.6 ms/帧 | Guest 标签 |
| 显示 | 60 Hz FIFO（面板支持 120 Hz，当前应用请求上限 60）；游戏请求 present interval = 1 | 2026-10-28 只读 `dumpsys display`；`present_interval` |
| 宿主 CPU | 8 核：6 × 3.53 GHz + 2 × 4.32 GHz | `cpuinfo_max_freq` |

### 1.3 判断：当前受限于依赖链（延迟），不是任何单一资源的吞吐

"CPU 30%、GPU 40%"并不代表还有 70%/60% 的余量可以直接用上。有三个结构性原因：

1. **Guest 并行宽度被固定为 3 个虚拟核。** `games/totk/runtime/threads.c` 中 `NCORE 3`，严格按 Horizon 规则调度：核掩码、优先级、优先级继承、在 SVC 处抢占。游戏会按 `GetCurrentCoreNumber` 无锁访问每核数据，所以这是正确性要求，不能放开。各线程的掩码：M `0x8001` 为 0x1；R `0x20004` 与 worker `0x58009` 同为 0x2；worker `0x6000a` 为 0x4。Guest 工作的正确分母是 3 个虚拟核，不是 8 个宿主核。
2. **翻译在 Guest 渲染线程 R 上同步执行。** R 每帧约 25 ms 翻译加约 7.4 ms 等消息，合计约等于一帧，说明 R 这条通道已经接近满载。同时 M 在 `WorkerMgr::sync` 和 SVC 0x34（WaitForAddress）上也在等待。
   **假说 H2**：M 和 R 处于锁步（lockstep），互相等待，帧时间等于两条通道串行段之和，而不是两者的最大值。
3. **宿主调频（DVFS）和调度从未测过。** 在约 30% 的负载下，调频策略可能让 R 所在的核运行在较低频率；R 是否被调度到 4.32 GHz 的 prime 核也不清楚。之前所有报告都写着"没有 scheduler sidecar"。

### 1.4 上限估算（只用来排序，不是承诺）

| 情形 | 帧时间下限 | 平均 FPS 上限（60 Hz FIFO） |
|---|---:|---:|
| 现状 | 约 33 ms | 约 30 |
| 翻译完全移出 R，翻译通道仍需 25 ms | max(M 通道, 25, GPU 10.6) | 约 40（前提是 M 通道自身工作 ≤ 25 ms，待 S1 测量） |
| 再加上翻译降本 30%（约 17.5 ms） | 由 M 通道或翻译通道决定 | 约 50–57 |
| 稳定 60 FPS | 所有通道都 < 16.7 ms | 60 |

60 Hz FIFO 下，30 到 60 FPS 之间的平均帧率由 1 和 2 个 vsync 间隔交替构成，会有抖动；120 Hz 模式可以让节拍更均匀（S5）。

---

## 2. 新增约束（v2 §3 全部继续有效）

- **禁止**修改 `NCORE`、核掩码、优先级或抢占规则，禁止使用 `TOTK_FREE_CORES`。允许的只有：让确实阻塞在宿主调用里的线程，按现有 `runtime_core_away()` 约定在阻塞期间让出虚拟核（S4）。
- 不 root、不锁频。ADPF 性能提示、对**本进程线程**的亲和性和优先级调整、显示模式请求属于允许的实验，但必须默认关闭并可热切换（S5）。
- 本 spec 所有基线测量都用启动属性显式开启 `publication_queue=1` 和 `invalidate_fast=1`，并在报告中写明。在 S0 闭合并获用户确认之前，不修改源码默认值。
- Thor 上的 GPU 时钟自测失败是独立的正确性问题，不在本 spec 范围内。但任何改动不得让 Odin3 和 Mac 上的 GPU report/时钟自测回归。

---

## 3. 阶段总览

```
S0 闭合 W1/W2 验收（小，可与 S1 并行）
S1 通道与关键路径测量（必须先做；只加诊断，不改行为）
 └─ D2 决策：按 S1 回答的问题排序 S2–S5
     S2 异步翻译：把翻译移出 R（H2 成立时的主线）
     S3 翻译降本（与 S2 并行；S2 之后它就是翻译通道的上限）
     S4 Guest 通道与虚拟核占用（仅当 S1 发现问题时）
     S5 宿主调度、调频与显示节拍实验（便宜，默认关闭）
```

通用停止条件沿用 v2 §4：每阶段最多两轮"实现 → ABAB"；出现崩溃、缺绘或配置漂移立即停止并写报告。

---

## S0：闭合 W1/W2 验收

1. **I2 每个 identity 只退休一次**：增加一个**只在诊断模式开启**的有界 identity 台账，以 serial 的 generation（`serial >> 12`）为键，记录 Initialize、按值副本退休请求和引擎实际删除，按 generation 对账。**不得**引入常驻的去重表，也不得改变按值副本语义。主机测试覆盖：重新 Initialize、按值复制后分别 Finalize、同地址复用、溢出。
2. **I5 生命周期矩阵**：对每个生命周期入口（`nvnMemoryPoolFinalize`、`nvnTexturePoolInitialize`、`nvnSamplerPool*`、`nvnDeviceFinalize`、shutdown）× 每种 pending 类型（RetireTexture / RegisterTexture / RegisterSampler），各写一个测试，检查入口执行前队列已空，且引擎看到的顺序正确。
3. 产出"改默认值"的单独 diff：`publication_queue=1`、`invalidate_fast=1`，附 S0 证据。**不自行应用**，交给用户确认。

验收：新测试全部通过；Mac 和 Odin3 probe 重链后，在 OFF、ON、ordered 三种模式下都通过。

---

## S1：通道与关键路径测量（只加诊断）

目标：用数据回答"每帧时间花在哪条通道上，哪条通道在关键路径上"，替代"CPU 30%"这种整体指标。

### 1.1 新增 `lane_status` debugbus（默认关闭，`on|off|status|reset`）

在每个成功 Present 的区间内，统计：

- **每个虚拟核**：有 owner 的时间、owner 持核期间实际在 CPU 上运行的时间、空闲时间，以及每个 owner handle 的持核时间。
  在 `core_put` / `core_take` 处累加持核时间，开销很低，可以常开（诊断开关打开时）。线程 CPU 时间用 `pthread_getcpuclockid` 读取，**只在 Present 边界采样一次**，不要在每次 take/put 时调用。可以复用 `threads.c` 中 `core_table()` 已有的字段（`core_held_ns`、`core_wait_ns`、`cpu_prev_ns`），但输出改为 debugbus 计数，不再打 stderr 表格。`TOTK_CORE_DIAG` 只在启动时读取一次，满足不了热切换要求。
- **虚拟核等待**：已就绪的 Guest 线程等待虚拟核的总时间和次数（`Sync.VirtualCore.Wait`），按等待方 handle 和当时的持核者 handle 汇总。
- **持核但未运行**：持核时间减去 CPU 时间，按线程汇总，并附上 `last_svc` 和 `last_hle`。这一项用于找出持有虚拟核却阻塞在宿主调用里的 HLE 路径。
- **R 通道**：翻译、等消息、drain、其他。
- **呈现通道**：present worker 的忙碌与等待、Acquire 等待。

### 1.2 宿主调度与调频（不 root）

- 用 `adb shell perfetto` 采集 10 s：`sched_switch`、`sched_wakeup`、`power/cpu_frequency`、`power/cpu_idle`，并开启本应用 atrace。用 ptracy 或 litep 的 `analyze_scheduling` 按 TID 拆出 running / runnable / sleeping，以及线程实际运行在哪个 CPU、当时的频率。
- 如果这个 user 构建不允许 perfetto 的 sched 事件，记录失败原因，退回到读取 `/proc/<pid>/task/<tid>/schedstat`（第二字段是在运行队列中等待的时间）和 `/sys/devices/system/cpu/cpu*/cpufreq/scaling_cur_freq`，在 Present 边界采样。
- 如果 KGSL 的 `gpuclk` 和 `gpubusy` sysfs 节点可读，一并采样；不可读就如实记录。

### 1.3 显示节拍

- 用 `dumpsys SurfaceFlinger --latency "<BOTW 图层名>"`（不需要 root）取实际上屏时间戳，统计 vsync 间隔直方图（1、2、3 个及更多间隔）和丢帧情况。
- 同时报告成功 native Present 的帧时间 p50 / p90 / p99。之后所有阶段的验收都增加 p90 和 p99，因为节拍抖动会直接影响体感。

### 1.4 关键路径重建

连续 ≥ 120 个完整源帧，**只使用显式边**，不用时间重叠推断：

- 已有的 Guest condition/address 等待链（enqueue → selected-notify → resume）；
- 消息队列 M → R；
- `WorkerMgr::sync` 中被等待的 worker 何时通知 M（需要时按 v2 §3 的规则补充窄 Guest 标签）；
- Submit 返回、Present、Acquire、fence 等待。

输出每帧的关键链：链上经过哪些通道、每段耗时多少；以及汇总结果："关键通道为 X 的帧占多少比例"。

### 1.5 S1 报告必须回答

- **Q1**：M 和 R 是否锁步？两者互相等待的时间各占多少？H2 成立还是不成立？
- **Q2**：M 在 `WorkerMgr::sync` 中等的是哪些 worker handle？这些 worker 当时在做什么：等虚拟核、等宿主锁、在 CPU 上运行，还是睡眠？
- **Q3**：三个虚拟核各自的占用率是多少？每帧的虚拟核等待时间是多少？
- **Q4**：有哪些线程"持核却不在 CPU 上"，对应的 SVC/HLE 是什么，每帧多少毫秒？
- **Q5**：R 翻译时运行在哪个核上、频率多少？runnable 延迟多少？
- **Q6**：实际上屏的 vsync 间隔分布如何？

验收：诊断开启时 FPS 下降 < 2%（ABAB）；两次独立采样得出的关键通道排序一致。

---

## D2：决策（S1 报告中必须写明走哪条分支）

| S1 结果 | 下一步优先级 |
|---|---|
| H2 成立（M 与 R 锁步），且 R 的翻译在关键链上的帧占多数 | S2 为主线，S3 并行 |
| R 不在关键链上，M 的 `WorkerMgr::sync` 等待的 worker 本身在等虚拟核或持核不运行 | 先做 S4，S2 暂缓 |
| 虚拟核接近饱和（三个核合计 > 85%）且等核时间显著 | Guest 侧已到 3 核上限；只有 S3（减少 R 在 Guest 侧的工作）和 S2 有意义，S4 无效 |
| R 运行在低频核上，或 runnable 延迟显著 | 先做 S5 的 ADPF/亲和性实验，成本最低 |
| vsync 量化明显（例如大量 2 间隔与 3 间隔交替） | 在 S2/S3 之外加做 S5 的 120 Hz 实验 |

---

## S2：异步翻译——把翻译移出 Guest 渲染线程（H2 成立时的主线）

目的：R 调用 Submit 后立即返回 Guest，由宿主翻译线程在空闲核上执行翻译，让 M、R、翻译、GPU 四条通道真正重叠。现有 `OrderedExecutor` 管道已经存在，但真实场景中提交 100% 回退（之前 −7% 到 −8%）。

### 2.1 第一步：测准入，不改行为

在 publication 和 invalidate 都开启的情况下，用 `ordered_translation=1` 跑一个 30 s 窗口，记录：
- `renderer_ordered_inputs status`：reason 5 子原因分布和首次现场；
- `ordered_status`：各 reason 计数、首个阻断 opcode；
- 按 opcode 统计的阻断次数。

### 2.2 准入阶梯

按阻断次数从高到低，**一次只处理一个子原因**。每处理一个，衡量的指标是**真实场景中被准入且确实异步执行的提交比例**，此时不看 FPS（部分准入只会带来 drain 和捕获开销，正是之前 −7% 的原因）。

阶梯门槛：≥ 50% → ≥ 90% → ≥ 98%。达到 90% 后才做 FPS 的 ABAB。

设计方向（来自 [ARCHITECTURE_REVIEW](ARCHITECTURE_REVIEW_20261027.md) 第三轮，实施前先写一页设计并在报告中附上）：
- 在提交侧维护独立的值型绑定状态，避免为读取继承状态而等待执行侧完成；命令和资源元数据持有稳定代次的引用。
- 可证明的 GPU→GPU 依赖留在 GPU 时间线上，不判定为必须同步；只对 CPU 可改写的输入做快照；CPU 可见的输出继续按 fence 和 Invalidate 的生命周期处理。
- 如果整批提交只被少量 CPU 可见操作阻断，就在有明确定义的同步点分段，同时保留状态继承、render pass、barrier、query 和别名的顺序。不能任意按 Draw 数拆包。
- Present 对已存在的宿主图像按资源和代次引用排队；只有 CPU-backed 的首次上传才需要捕获输入。
- **发布队列与有序模式的衔接**：当前有序模式下发布队列是禁用的。要么让 drain 的执行体进入 `translation_owner` 的 FIFO（与 Submit 保序），要么证明有序模式下保持同步发布不会重新引入锁等待。二选一，并写测试证明顺序正确。

### 2.3 正确性测试（新增）

- Submit 之后 Guest 立即改写 UBO 或顶点数据，翻译结果必须仍使用提交时的内容；
- 翻译还在途时，Guest 读取 CPU 可见的 report 或 event（必须等到真实完成）；
- 翻译在途时同地址复用纹理或内存池；
- 翻译在途时 Present、窗口 Finalize、shutdown；
- 准入失败与成功交替时的状态继承。

现有 probe 的 ordered 模式全部保留，Mac MoltenVK 与 Odin3 Turnip 都要通过。

### 2.4 S2 验收

| 指标 | 通过条件 |
|---|---|
| 稳态异步覆盖率（准入且执行） | ≥ 95% |
| R 线程上 Submit 的 elapsed | 下降 ≥ 80% |
| 每帧捕获开销（`NVN.Ordered.Capture`） | < 被移走的翻译时间的 20% |
| 整帧 FPS（ABAB） | ≥ +10% |
| 帧时间 p99 | 不劣化超过 10% |
| 在途队列深度 | ≤ 1 帧（有界，避免输入延迟增加）；报告队列深度分布 |
| 新出现的等待（Acquire、SyncWait、drain） | 列入等待转移表，不得抵消收益 |

---

## S3：翻译降本（沿用 v2 W3，新增两点）

v2 W3 的方法和验收（线程 CPU 下降 ≥ 8%，FPS ≥ +2%）不变。新增：

1. 报告中附上 S1 测得的 R/翻译线程运行频率，区分"代码变快"和"调频变化"。
2. 已知热点：R 的 self 时间中，Draw 约 17.1 ms/帧、CpuRead.Preflight 约 3.9 ms/帧，单次 Draw 约 5 µs，每帧约 3800 次。先用 `profiler_detail start 32` 抽样拆分 Draw，再优化排名前两位的子项。

S2 完成后，翻译通道本身就是 FPS 上限（约 25 ms 对应约 40 FPS），所以 S3 的价值会随 S2 的推进而上升。

---

## S4：Guest 通道与虚拟核占用（仅当 S1 的 Q3/Q4 显示问题时）

- 对"持核却不在 CPU 上"排名前列的 HLE/SVC 路径：如果它们在宿主上阻塞（文件 IO、音频、宿主锁、GPU 等待），按现有约定用 `runtime_core_away()` / `runtime_core_back()` 包裹阻塞段，与 `applet_host.c` 和 `HostWait` 的做法一致。**只能包裹 Horizon 本身就会切换线程的阻塞点**；纯计算型 HLE 不得让出虚拟核，否则会改变 Guest 可见的调度。
- 对 M 等待的 worker：如果它们在等虚拟核，查明是哪个线程持核、为什么持核（结合 `last_svc`），在报告中给出因果链，然后再决定是否修改。
- 验收：持核未运行时间下降，相关 worker 等核时间下降，M 的 `WorkerMgr::sync` 下降，整帧 FPS ≥ +2%（ABAB）。

---

## S5：宿主调度、调频与显示节拍实验（默认关闭，每项单独 ABAB）

1. **ADPF**：用 `APerformanceHint`（NDK，API 33+；Odin3 为 API 35）为 R/翻译线程和 present worker 建立 hint session，目标时长取当前帧预算，每帧上报实际耗时。开关 `renderer_hint on|off`。报告实际频率变化、FPS 变化和热状态变化。
2. **线程放置**：把翻译线程（以及 S2 之后的宿主翻译 worker）和 present worker 的亲和性设到 prime 核（`sched_setaffinity` 只作用于本进程线程）。不改 Guest 线程的亲和性，因为它们受虚拟核调度管理。开关 `renderer_affinity off|prime|auto`。
3. **120 Hz 显示模式**：通过 `Surface.setFrameRate(120, …)` 或 `preferredDisplayModeId`（Odin3 的 mode 1 为 120 Hz）请求 120 Hz。报告 SurfaceFlinger 上屏间隔直方图、p90/p99、功耗与热状态变化。注意 120 Hz 不改变渲染吞吐，只影响量化方式；如果 S1 的 Q6 没有显示量化问题，就跳过这一项。

每项验收：FPS ≥ +2% 或 p99 改善 ≥ 10%，同时热状态没有恶化（`Thermal Status` 前后一致，并记录温度）。持续 5 分钟以上的长窗口至少跑一次，排除短窗口内的升频假象。

---

## 4. 测量规范补充（在 v2 §6 基础上）

- 基线配置固定为：`publication_queue=1`、`invalidate_fast=1`、`ordered_translation=0`（S2 除外）、720p 手持、内部 1×、realtime=1、PSO workers 4、显示 60 Hz（S5 除外）。
- 每份报告除了合并 FPS，还要给出帧时间 p50 / p90 / p99，以及 SurfaceFlinger 上屏间隔直方图（S1 之后）。
- 涉及调度或调频的实验，至少有一个 ≥ 5 分钟的窗口，并记录开始和结束时的 `Thermal Status` 和温度。
- "利用率"一律分通道报告：3 个虚拟核、R/翻译通道、呈现通道、GPU。不再用整机 CPU 百分比作为瓶颈判断依据。

## 5. 明确不要做

- 不要放开虚拟核数量、核掩码或优先级规则。
- 不要在异步覆盖率低于 90% 时做 FPS 结论，也不要默认开启 `ordered_translation`。
- 不要继续追加去锁类改动，除非 S1 的 `frontlock_status` 显示某个 API 在关键链上。
- 不要用"CPU 还有空余"作为增加 PSO worker 或其他后台线程的理由；先证明新线程处在关键路径上。
- 不要锁频、root，也不要为了跑分把调频实验默认打开。
- 不要把 Mac、`BOTWRecompAndroid` 上游或标题界面的 FPS 当作目标或证据。
