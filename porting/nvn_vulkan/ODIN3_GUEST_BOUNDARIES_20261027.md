# Odin3：Guest 标记实机对照与两阶段瓶颈复核

日期：2026-10-27。

## 结论

指定诊断 APK 已完成原位安装、四个有效无插桩窗口、五份连续 PROF、独立解码核验和数据审计。**仍未通过性能验收：串行加权 20.928 FPS，有序 19.406 FPS，观测差异 −7.268%。**

这轮取得了三个比此前更明确的结果：

1. 真实提交的第一个异步阻断点是 **`OP_REPORT_COUNTER`（opcode 5），第 0 个命令句柄、字节偏移 8**，紧跟 `OP_BEGIN`。两个有效有序窗口的 914/896 次新增回退全部如此；异步 Translate 仍为零。
2. Translate 线程的主要 GuestCondition 等待来自 **`nn::os::ReceiveMessageQueue`**。同一诊断窗口内，50 次完整配对都由同一消息生产线程选中通知。时间主要在通知之前，不是在通知后争抢虚拟核心。
3. 第一阶段准备缓存开启/关闭时，抽样 Draw 内都约有 **7.48 次 Snapshot**，Descriptor 和 Snapshot 工作量没有明显消失。继续堆 key 缓存缺少收益依据。

同时，Guest 标记观测到约 **22% 的源帧间隔增长**，必须缩窄诊断范围后再用于精细耗时归因。它仍能提供显式通知关系，不能当作无插桩性能基线。

## 设备与数据保留

- Odin3：`6edb0593`，AYN / sun，Android 15 / API 35，arm64-v8a。
- 包：`org.macnative.android.botw`。
- APK SHA-256：`dd7dba23362eea7d58db4c75627927038004d187da3e0e47e040a309f1a4747b`。
- 安装前 APK SHA-256：`21151c0fd10850ed5f567364b4263bd30e2c1f890d3621b8ec84024a46dc6286`，已拉取备份并校验。
- Turnip SHA-256：`e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`，未替换。
- BOTW 1.6.0 main Build ID：`8e9978d50bdd20b4c8395a106c27ffde`。
- 沿用已有存档、720p handheld、1×、realtime 配方和缓存；没有注入移动/相机输入。

安装前备份 APK、偏好、存档、账号、overlay 及缓存。备份 tar SHA-256：`17896c5830e6dd4317639c79113d48fe76beb1ed215e8a76613812d9d4575e70`。安装前后审计的 **78 个文件哈希全部相同**。

结束时 78 个文件仍在，无新增/删除；77 个哈希相同，仅 `option.sav` 的 4 字节变化，长度仍为 344。变化字段含义没有验证，不将它称为“只是计时”。前后字节均保留，**没有恢复或覆盖存档**。新生成的 profiler 文件另行保留，不属于这 78 个用户数据/缓存审计文件。

没有 Root、锁频、清数据、卸载、删缓存或操作另一台 Thor。最终关闭 Guest/detail/ring/capture，恢复 `debug.mng.ordered_translation` 和 `debug.mng.autoload` 的原空值，只停止本轮进程 6163，保留新 APK。后续此项目的 Odin3 验证已获用户持续授权，不再逐包重复询问，但不扩大上述操作边界。

## 无插桩对照

每个有效窗口约 45 秒，要求已打开现有存档、持续 20 秒满足 >=1000 draws、有 dispatch、无新 shader/PSO、状态新鲜且 GameActivity 在前台。FPS 采样期间 profiler recording、Guest 标记、detail、perf sampling、preparation validation 均关闭。

| 窗口 | 模式 | PID | presents | 秒 | FPS | draws 中位数 | dispatch 中位数 |
|---|---|---:|---:|---:|---:|---:|---:|
| OFF A | 串行 | 30308 | 955 | 45.679 | 20.907 | 3688.5 | 14 |
| ON A retry | 有序 | 32197 | 892 | 45.746 | 19.499 | 3723 | 14 |
| ON B | 有序 | 4270 | 883 | 45.718 | 19.314 | 3710 | 14 |
| OFF B | 串行 | 6163 | 957 | 45.684 | 20.948 | 3653.5 | 14 |

原 ON A 在采样中不再满足场景/暖机门槛，被完整保留但不计入结果。旧验证助手未保存触发拒收的那一个采样值，因此无法事后确定是哪条子条件失效。重试使用同一已稳定进程 32197，重新满足原门槛，没有降低阈值。其余三组独立启动，所以这不是无异常的严格 ABBA。

度量为 successful native Vulkan presents/s，不是屏幕合成时间戳或独立标定的唯一 Guest 帧。截图为同一存档区域，存在 idle 动画和相机距离差异，未建立精确地图坐标/相机姿态匹配；未锁频。因而 −7.268% 是本轮观测，不是通用或严格因果回归系数。四个有效窗口未发现 fatal、device lost 或明显新增黑屏/缺绘；不代表全游戏正确性或输入延迟验收。

本轮只对同一新 APK 的模式进行对照，没有将旧 APK 重新装回作严格二进制 A/B。不能据此量化短锁修正本身的收益。

## 真实 opcode 阻断

| 有序窗口 | 异步提交增量 | 回退增量 | opcode 5 增量 | drain 增量 |
|---|---:|---:|---:|---:|
| ON A retry | 0 | 914 | 914 | 93991 |
| ON B | 0 | 896 | 896 | 92312 |

计数快照在 FPS 窗口前后相邻读取，边界不与每一个 present 严格重合。最近第一阻断位置均为 handle 0 / offset 8；无 opcode 桶溢出或省略。

`nvn_obj.h` 将 opcode 5 定义为 `OP_REPORT_COUNTER`。固定 D2R 执行器的实现为 timestamp/occlusion report 建立 GPU query 和 `PendingReport`，之后才完成 CPU 可见结果写回。保留的实机日志也显示稳态 timestamp report 约 5 次/帧、pending reports=2。

**阻断点不等于耗时热点：**不是说这条 report 执行了 28 ms，而是捕获扫描一开始就因此拒绝整批，后续 Draw 仍在前端锁内同步翻译。也不能将它简单加入允许列表：仍需保住输出存活期、GPU 完成语义、CPU 读回可见性，以及继续扫描后可能遇到的其他不支持命令/可写输入。

## Guest 消息等待链

五份 trace 都来自 OFF B 的 PID 6163。Translate 实际在线程 **6367**；header 的 main_thread_id **6452** 是 presenter，不能混用。

Guest-only 捕获覆盖 51 个完整 source frames；显式配对 3518 次跨线程条件等待，排除 75 个超出完整帧窗口的等待和 13 个缺失/重复端点项。不能把各线程等待总和相加当帧时间。

Translate 线程的 50 次完整等待属于同一调用链，通知线程均为 **6279**：

| 完整等待的平均阶段 | ms/次 |
|---|---:|
| 登记等待 → 选中通知 | 24.994 |
| 选中通知 → 实际 wake | 0.0198 |
| wake → 返回（含虚拟核心重新获取） | 0.0798 |
| 全部等待 | 25.094 |

中位数 19.796 ms，P90 42.767 ms。这里是开启标记后的经过时间，不是 on-CPU，也不是无插桩的真实成本。

身份固定的 Guest 位置：

- 等待 SVC：`sdk+0x37eb3c`。
- FP 返回记录：`sdk+0x32637c`，落在清单 vendor symbol `nn::os::ReceiveMessageQueue(unsigned long*, nn::os::MessageQueueType*)` 内。
- 上层返回线索：`main+0x126db3c`、`main+0x126cf74`。
- 通知 SVC：`sdk+0x37eb44`。
- 生产者返回线索：`main+0x126dc00`、`main+0x124ed74`、`main+0x1614c50`、`main+0x1614950`。

SDK Build ID：`fcf93a41b08c5193a1410d262602dc621617c56f`；模块清单 SHA-256：`30513eed53bc4ae9bd1539c9e28dbffdb859b9c2c054fefd05aa9e0de3054f38`。main 的未命名偏移没有按最近符号归属，没有为它们发明游戏子系统名称。FP 记录只作有界返回线索，不是完整验证的 unwind。

独立的准备缓存开启 detail 捕获再次观察到同一等待/通知链：48 次完整等待，全部由 TID 6279 通知，平均 26.770 ms，其中通知前 26.631 ms。

这证明了消息接收与真实通知者的关系，**尚不能证明生产线程在这 25 ms 内一直计算、哪项任务负责产生消息、或这些等待都能消除**。它也不是“在等 GPU”的证据。`threads` 快照部分返回 busy，未将这种缺失解释为线程空闲。

## 标记扰动

相同进程、串行模式、准备缓存开启，连续采三份 3 秒文件：

| 捕获 | Guest 标记 | 完整 source frames | 平均源帧间隔 ms | Translate ms/frame | GuestCondition ms/frame |
|---|---|---:|---:|---:|---:|
| basic A | OFF | 62 | 47.286 | 27.772 | 19.148 |
| guest | ON | 51 | 57.673 | 32.069 | 24.898 |
| basic B | OFF | 63 | 47.214 | 26.615 | 20.229 |

Guest ON 的平均帧间隔比两侧均值长约 **22.06%**。这是短窗口观察，不能拆成精确的单个标记成本，但足以说明该模式有明显扰动。基础 tracing 三组都开启，与 45 秒完全不录制的 FPS 窗口不同。

Guest-only 文件含 1,042,310 条 Guest 范围记录。其中 61,236 个边界是 SVC `0x0B`（SleepThread/yield）；大多数来自其他线程而不是 Translate 线程。因为没有记录 x0 参数，不能区分正时长 sleep 和 0/−1/−2 yield，更不能由次数断言这些调用都是浪费。

后续标记应缩窄为目标条件等待/通知和特定调用点，避免全量记录 SleepThread 的四层 FP 线索。低扰动前不能拿 ON 窗口的 32 ms Translate 作为新的正常基线。

## 第一阶段缓存：命中后仍做什么

基础 trace 中，Translate 线程每 source frame：Draw 18.681–19.556 ms、Preflight 4.324–4.415 ms、Translate 26.615–27.772 ms。父子作用域重叠，不相加；无 sched/KGSL sidecar，仍不代表纯 CPU 执行时间。

另两份捕获开启 Guest 标记和每 32 Draw 一次的整次 detail，分别关闭/开启第一阶段准备缓存。只把完整落在 `NVN.Draw.Sample` 内的子作用域归入该样本，排除 Dispatch 等非 Draw 的完整 detail：

| 每个抽样 Draw | plans ON（5593 个） | plans OFF（5916 个） |
|---|---:|---:|
| Draw.Sample μs | 11.775 | 11.543 |
| Descriptor inclusive μs | 5.211 | 5.048 |
| Snapshot inclusive μs | 3.360 | 3.297 |
| Snapshot 调用数 | 7.480 | 7.476 |
| Encode μs | 1.528 | 1.495 |
| PipelinePrepare μs | 0.724 | 0.717 |
| 上述 Descriptor/Encode/PipelinePrepare 并集外 μs | 4.312 | 4.282 |

Snapshot 嵌在 Descriptor 等范围内，不能把表中 inclusive 时间相加。Encode 也不是纯 Vulkan 驱动时间；并集外时间不是 idle，可能包含其他准备和采样本身。详细采样会增加被抽中 Draw 的成本，不将这些比例直接乘回无插桩帧时间。

开启准备缓存没有明显减少 Snapshot 次数或采样 Descriptor 耗时。这支持把下一项 CPU 优化放在**命中后的数据快照、版本/脏区和绑定消费路径**，而不是再新增一层 key 缓存。任何复用仍必须保留当前内容、GPU 写回和别名验证，不能把“地址没变”当“数据没变”。

## 完整性与分析边界

五份文件的设备/本地 SHA、PID、capture ID 与模块身份均核验，capture_complete=true，容器完整、丢块标记=0。

常驻 Litep MCP 仍为 `20261005.all4`，磁盘当前包为 `20261016.update1`。旧 MCP 把后四份捕获起点的残留未配对 end 标为 truncated。没有直接忽略：

- 原始文件再通过当前已安装包的公开 `prof_stream.visit_file` 流式核验；五份均 payload complete、truncated=false、interior scope_pairing_errors=0、chunks_skipped=0。
- Guest-only 旧索引的 23 个未配对 end 全在完整帧窗口之前；用于通知关系的窗口内为 0。detail ON 同样验证。
- 仍保留起点/终点未闭合作用域及 GPU orphan/incomplete-zone 诊断，不称为所有端点无损。
- SQL 只读分析；通知按显式 epoch/wait ID/waiter 配对，counter 归属按 parent sequence，不靠时序相邻猜边。
- 没有校准 CPU/GPU 时间轴或完整关键路径，也没有完整纯 Guest 帧函数覆盖。

## 实施优先级

1. 先处理 counter report 的有序输出所有权，再继续发现后续阻断点；不盲目扩大 allowlist，也不跳 report/fence。
2. 将 Guest 标记缩窄到已测得的消息等待/通知链，查明生产者发布前在做什么；保留 OFF/ON/OFF 扰动检查。
3. 优化命中后的快照和绑定准备；以实际复制量、调用数、经过时间及无插桩 FPS 验证，不以缓存命中计数验收。

本轮验证期间未修改 renderer/runtime 源文件，仍是指定 SHA 的同一 APK；以上是证据导出的改造方向，不是已实现的提速。

## 私有证据

根目录：`native-switch-study/android-translate-20261026/odin3-guest-boundaries-20261027/`。

- `deployment.json`、安装前 APK/数据 tar、前后 inventory：部署与保留证明。
- `off_a/on_a/on_a_retry/on_b/off_b`：有效及失败窗口、状态、日志、截图。
- `cadence-summary.json`、`observer-summary.json`：两种不同口径的对照。
- `basic_a/guest/basic_b/detail_plans_on/detail_plans_off`：原始 PROF、sidecar、模块快照、索引、当前 reader 核验。
- `guest/guest-analysis.json`、`detail_plans_on/guest-analysis.json`：配对、调用位置、vendor 符号及被拒绝端点。
- `detail_plans_*/sampled-draw-summary.json`：正确样本分母的 Draw 明细。
- `preservation-final.json`、`changed-after-0.bin`：收尾与运行后 option.sav。

采集/分析脚本位于 `MacNativeGames/android/build/nvn-guest-boundaries-20261027/device_validation/`，与私有数据分离。所有读取私有文件的 Python 均用 `-I`。分析过程中的脚本语法错误、一次无索引 end-id 查询超时、分析预算拒绝均保留日志；修正工具后重跑，没有删除失败采集或弱化实机门槛。未提交或推送。
