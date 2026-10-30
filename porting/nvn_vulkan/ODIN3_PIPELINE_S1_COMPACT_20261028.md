# Odin3 S1：紧凑调度记录、采样扰动与测量顺序

日期：2026-10-28。接续 `ODIN3_PIPELINE_S1_RETRY_20261028.md`，遵循
`SPEC_PIPELINE_PARALLELISM_20261028.md`。本轮只改变诊断编码和测试顺序，
没有改变 NVN/Vulkan、Guest 调度、虚拟核数量或渲染行为。

## 结论

- 已降低 Lane PROF 记录开销。两份各 120 个连续完整源帧的 ROI 中，
  `Runtime.LaneSample` 为 **5.472 / 5.402 ms/源帧**；上轮为
  17.412 / 18.119 ms/源帧，范围内 elapsed 下降约 **69%**。
  这是诊断范围耗时的跨轮观察，不是游戏帧率提升，也不是 on-CPU 时间。
- 编码完整性验证通过：两份文件分别验证 **208 / 204** 个完整包、
  **294,964 / 290,667** 条调度事件。三个核的持核和空闲总量与运行时
  `Lane.Core*.HeldNs/IdleNs` **逐纳秒一致**。第二份末尾有一个未结束包，
  已明确排除；没有修复、补造或默默忽略数据。
- Odin3 稳定无录制窗口仍约 **29.5 FPS**。S1 的 `<2%` ABAB 门槛
  **本轮仍不能验收**：录制前后出现持续几十秒至一分钟以上的性能状态漂移，
  整组结果分别为 −9.13% / −6.83%，不能把这个差值直接当作诊断的因果开销。
- 找到一个独立的构建问题：当前 Android Debug 构建的运行时、MNG profiler
  包装层使用 `-O2`，但 Foundation `foundation_profiler_ring` 和
  `profiler-sdk` 编译命令没有优化参数，实际是 Clang 默认 `-O0`。
  这提供了下一轮明确的测量目标，但尚未通过 A/B 证明它造成了多少开销。
- **D2：继续停留在 S1。** 两轮“实现/测量修正 → ABAB”已完成，按 spec
  本轮停止继续调参。不默认开启有序翻译，不进入 S2/S4 行为改造。

## 身份与设置

| 项目 | 值 |
|---|---|
| 设备 | Odin3 `6edb0593`，仅此设备 |
| 包名 / PID | `org.macnative.android.botw` / `25813` |
| APK SHA-256 | `9bcedb3e6e53cb47a19213d38d8f1ef9ee458662c4ae6c784b0007304c30fb65` |
| ELF Build ID | `e2a858e389394be884075fc005c69875609a7690` |
| BOTW main Build ID | `8e9978d50bdd20b4c8395a106c27ffde`，原始清单保留尾部 padding |
| Turnip SHA-256 | `e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d` |
| 配置 | publication=1、invalidate=1、ordered=0、720p 手持、1×、realtime=1、PSO workers=4 |
| Present | interval=1；本轮没有切换显示模式 |
| 场景 | 自动读最新存档；持续暖机 Draw/Dispatch 门槛通过；同 PID 无输入注入 |

场景门槛证明存档已加载并持续渲染，不能代替地图坐标/相机姿态的语义证明。
FPS 都是成功 native Present 节拍，不能称为 SurfaceFlinger 实际上屏帧率。

证据目录（仓库外，不提交原始 PROF/存档/二进制）：
`/Users/bytedance/workspace/native-switch-study/android-translate-20261026/pipeline-s1-compact-20261028/`。

## 实现

`porting/runtime/lane_packet.h` 定义版本化记录。原先每个事件一个 scope 加
八个 counter，改成每个事件一个有界字段字和原始 MONOTONIC 时间字。
每帧统一封装；原始时间、row/core/kind、核掩码、SVC、完整 32-bit HLE 字段
均保留。线程身份在录制代次或 Lane 代次变化时完整登记，随后只登记新增行。
包携带初始 owner、完整区间、事件数、拒绝数与字节序明确的校验和。

生产侧仍只向有界队列写值；不在 Guest 调度锁里格式化、发 PROF 或读取 CPU
时钟。Present 的 CPU 时钟采样和事件回放逻辑不变。Mac/Android 共用这份实现。

`my_mcp_tools/dev_tools/mcp/litep/native_lane_packets.py` 与公开工具
`analyze_guest_lanes` 校验 scope parent、线程身份、字段顺序、计数、时间范围
和校验和，再重建持核区间。保留未知 owner、文件边界和缺失定义的诊断。
使用源码启动的 MCP 完成真实文件验证；没有宣称已安装全局工具发行版。

`android/tools/benchmark_publication.py` 已把所有录制移到四个 cadence 窗口
之后，并记录窗口/录制的时间戳。本轮第二组证据确认所有录制均晚于四个窗口。

## ABAB：为什么不能宣布门槛通过

各窗口 60 秒、同一进程、录制期间的 FPS 不混入无录制窗口。

| 运行 | OFF A | ON A | OFF B | ON B | 合并 OFF → ON |
|---|---:|---:|---:|---:|---:|
| 原顺序，ON 窗口后穿插录制 | 29.503 | 29.477 | 36.838 | 30.804 | 33.167 → 30.139（−9.13%） |
| 改为最后集中录制 | 33.811 | 29.458 | 29.472 | 29.508 | 31.644 → 29.483（−6.83%） |

第一组在 ON A 后录制，OFF B 的十二个 5 秒子窗持续约 35–39 FPS；
ON B 前两个子窗仍为 38.83 / 35.29，之后回到约 29.5。
第二组紧接上一组录制之后开始，OFF A 前八个子窗仍偏快，最后四个回到约
29.5；后三个完整窗口稳定约 29.5。Thermal Status 均为 0。

因此，测试顺序修正是必要的，但仍需在下一轮开始前增加**事先定义的恢复/稳定
门槛**。不能事后删除 OFF A 或挑选后三个窗口，将不完整 ABAB 写成通过。
DVFS、线程调度或其他状态变化尚未用同次 sidecar 区分，不能仅凭 Thermal
Status 为 0 宣称没有调频影响。

## 两份完整采样

第一组 ON A / ON B 分别选取 SDK 源帧 80–199 / 289–408，各 120 帧。
排除文件头残留和 >200 ms 的跨录制桥接区间，再取固定内部连续窗口。
边界外 scope 不计入完整调用耗时，完整调用数保留在 `roi-scopes.json`。

| 指标 | A | B |
|---|---:|---:|
| 录制 ROI native FPS | 26.060 | 25.755 |
| 帧时间 p50 / p90 / p99，ms | 37.713 / 45.604 / 57.413 | 37.835 / 48.157 / 62.213 |
| LaneSample elapsed / 源帧，ms | 5.472 | 5.402 |
| LanePacket elapsed / 源帧，ms | 5.007 | 4.879 |
| NVN.Translate elapsed / 源帧，ms | 27.672 | 28.249 |
| NVN.Draw 累计 elapsed / 源帧，ms | 18.415 | 18.820 |
| CpuRead.Preflight 累计 elapsed / 源帧，ms | 4.236 | 4.405 |
| WorkerMgr::sync 累计 inclusive / 源帧，ms | 19.972 | 20.148 |
| R ← M 显式完整等待链数量 | 119 | 119 |
| R ← M 等待 / 源帧，ms | 8.641 | 8.768 |

范围存在包含关系，不能把这些行相加当作整帧时间。完整录制仍使节拍下降，
所以这里只验证编码和受扰动状态下的通道画像，不作未经扰动的关键路径结论。
两份容器完整、chunks_skipped=0、scope_pairing_errors=0；原始边界、GPU
不完整区间以及 B 的两条未配到 enqueue 的边界记录保留在工具结果中。

## S1 问题的当前答案

- **Q1：H2 尚未闭合。** R 等 M 的显式边可重复；尚没有反向 M 等 R 的完整
  因果链，不能用二者范围重叠证明锁步。
- **Q2：** M 的 Address 等待存在来自 worker `0x58009` / `0x6000a` 的显式
 通知；另有无 selected-notify 的记录，保留结果码，不能补成某个 worker。
  WorkerMgr 范围与 worker 的逐帧调度状态尚未联合闭合。
- **Q3：** 第二组无录制 ON 窗口中，核 0/1/2 持核占比分别为
  **32.49/39.85/81.41%** 与 **31.81/39.36/81.02%**。三核平均约 51%，
  不支持“整体三核已饱和”。两个 worker 等核约 **0.70/0.92 ms** 与
  **0.68/0.89 ms/源帧**。所有窗口均无 event overflow、丢失 Present。
- **Q4：** R 每帧线程 CPU 为 **24.39 / 24.47 ms**，持虚拟核仅
  **0.101 / 0.099 ms**；翻译已经让出了虚拟核。持核未运行只能给上下界；
  `last_svc/last_hle` 不是当前阻塞点，不能据此增添 HostWait。
- **Q5：** 本轮没有新的 scheduler sidecar，不能沿用上轮频率作为当前 ROI
  的频率，也不宣称已测得翻译专属 runnable 延迟。
- **Q6：** 本轮没有新的对齐 SurfaceFlinger ROI；以上是 native 节拍分位数。

记录时三个核持核占比升至约 62/68/99%，明显不同于无录制状态，也说明不能
把重采样结果直接拿来决定修改虚拟核调度。

## 下一轮的具体顺序

1. 在 MNG 共同的 CMake 集成层明确 profiler 基础设施的优化配置，保留符号
   和断言；同时覆盖 Mac/Android。只改变宿主目标的构建参数，不修改 Foundation
   源码、不切换 SDK 版本。先用真实 SDK probe 和受控微基准确认收益。
2. ABAB 前先执行事先定义的恢复/稳定门槛，全程不穿插录制；记录恢复曲线，
   未稳定就终止比较，不能无限重试。分别测无 PROF 诊断与完整录制开销。
3. 若无 PROF 诊断仍超门槛，拆分事件生产、回放/统计和 CPU 时钟采样成本；
   当前每帧 CPU 时钟读取自身仅约 29 µs，不能预设它是全部开销。
4. 开销合格后，再做同场景、同次时钟校准的 scheduler + Litep 两份采样，
   补齐 M/worker/R 的关键链，按 D2 选择 S2/S3/S4/S5。

## 验证与收尾

- Mac/Android 构建通过，Android lint 通过；27 个底座测试、2 个 profiler
  测试通过；新编码 UBSan 通过；Android 真实 SDK probe 通过。
- 显式重链后，Mac MoltenVK 与 Odin3 Turnip 的 OFF、ON、ordered 六个
  Vulkan probe 全部通过。
- Litep 77 项相关回归通过；源码 MCP 的公开接口已读实际 PROF 完成校验。
- 安装前后 78 个设置/缓存/存档文件哈希一致；结束仍为 78 个，只有游戏正常
  改写的 `option.sav` 不同，未回滚存档。
- 所有诊断关闭、启动属性恢复，游戏 PID 25813 保持运行。结束健康检查约
  29.32 FPS，仅作健康检查。未 root、未换驱动、未清数据、未修改 NCORE。
- 阶段差异：`android/build/pipeline-s1-compact-20261028/change.diff`。
  未提交、未推送；源码默认 publication/invalidate 仍为 0。
