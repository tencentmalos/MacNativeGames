# Odin3：counter report 输出生命周期与窄 Guest 标记验证

日期：2026-10-27。

## 当前实机结论

**counter report 已不再是首个无条件阻断点，但 BOTW 的整批 Translate 仍未进入异步；性能验收未通过。** 同一新 APK 的四次无插桩窗口中，串行加权 20.938 FPS，有序 19.452 FPS，观测差异 −7.100%。不能称为提速，也不能将它与上一 APK 的跨轮数字解释成严格二进制 A/B。

受约束输出实现、本地测试及生命周期协议见 [COUNTER_REPORT_20261027.md](COUNTER_REPORT_20261027.md)。这里记录真实游戏，不以主机合成的异步报告计数替代实机覆盖率。

## 身份与部署

- 设备：Odin3 `6edb0593`，AYN / sun，Android 15 / API 35。
- 包：`org.macnative.android.botw`。
- APK SHA-256：`0bfb2ed42694cf87d9a82a4f53991bd3112851aade1114a17bc7729445c40b74`。
- 旧 APK：`dd7dba23362eea7d58db4c75627927038004d187da3e0e47e040a309f1a4747b`，安装前已完整备份并校验。
- BOTW 1.6.0 main Build ID：`8e9978d50bdd20b4c8395a106c27ffde`。
- Turnip：`e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`，未更换。
- 720p handheld、1×、原 realtime 配方、第一阶段 preparation plan 开启；未更换存档、注入相机或移动输入。

原位安装前备份 APK、偏好、存档、账号、overlay 与缓存。数据 tar SHA-256：`5945ec296dab0bbf6273c3e0d4e78e31a08048d296fc5859c920223033aaf638`。安装前后审计的 78 个文件哈希全部相同。

## 同 APK 无插桩对照

按 OFF / ON / ON / OFF 顺序，四次独立启动，各自连续满足 20 秒暖机／场景门槛后采约 45 秒。要求已打开现有存档、>=1000 draws、有 dispatch、无新 shader/PSO、状态新鲜、GameActivity 在前台。四组均一次通过，未降低门槛。

| 窗口 | 模式 | PID | presents | 秒 | FPS | draws 中位数 |
|---|---|---:|---:|---:|---:|---:|
| OFF A | 串行 | 4968 | 949 | 45.711 | 20.761 | 3643.5 |
| ON A | 有序 | 6758 | 896 | 45.721 | 19.597 | 3655.0 |
| ON B | 有序 | 8554 | 882 | 45.684 | 19.306 | 3654.0 |
| OFF B | 串行 | 10383 | 966 | 45.747 | 21.116 | 3654.5 |

四组 dispatch 中位数均为 14。Guest/detail/ring/capture/perf sampling/preparation validation 均关闭。FPS 为 successful native Vulkan presents/s，不是 compositor 时间戳，也没有独立唯一 Guest 帧计数；截图中系统 overlay 的瞬时 FPS 不是这里的平均值。

四张截图已人工检查，均在同一初始房间与相近相机位置，存在 idle 动画和动态效果差异；没有建立精确坐标／像素等价。未看到明显新增黑屏或缺绘。各窗口日志中未发现 fatal、device lost、SIGSEGV/SIGABRT、未捕获输入或失效 report 输出断言。没有全游戏正确性或 GPU validation layer 验收结论。

## 越过 report 后的真实阻断

| 相邻状态快照增量 | ON A | ON B |
|---|---:|---:|
| 异步 Translate 提交 | 0 | 0 |
| 全部回退 | 909 | 891 |
| 原因 4：未知 opcode | 0 | 0 |
| 原因 5：Draw 输入捕获检查 | 909 | 891 |
| 原因 8：报告所有权／范围 | 0 | 0 |
| 原因 9：报告输入／输出冲突 | 0 | 0 |
| 含 report 的异步提交 | 0 | 0 |

两个稳态窗口最近一次回退位置都是 **`OP_DRAW_IDX`（opcode 26），handle 0、offset 12560**。此前 counter report 在 offset 8 就失败；现在扫描确实继续到了后续 draw，但整批仍被安全门拒绝，报告最终仍随同步回退执行。

原因 5 由多个具体检查共用：shader binding／variant、可写 storage、buffer/texture 输入收集、预算、索引范围以及别名等。本轮证据不能区分具体子条件，不能将它直接说成“已证明是 writable SSBO”或“32 MiB 上限”。没有为了通过而放宽这些检查。

累计状态还保留加载阶段的 opcode 7 `OP_UPDATE_UB` 与 opcode 38 `OP_CLEAR_TEX` 回退；它们在上述稳态窗口内增量为零。原 `ordered_first_*` 字段仍对应最近的原因 4，所以其中残留 opcode 7/offset 9144，**不是当前稳态的第一阻断点**；本轮新增的 `ordered_last_*` 才对应最新任意原因回退。

## 窄 Guest 标记：扰动下降，通知链保留

四个无插桩窗口结束后，在同一串行进程 PID 10383 中单独采三份 3 秒 PROF，detail 关闭、准备缓存开启：

| 捕获 | Guest 标记 | 完整 source frames | 平均源帧间隔 ms | Translate ms/frame | GuestCondition ms/frame |
|---|---|---:|---:|---:|---:|
| basic A | OFF | 60 | 49.070 | 27.792 | 20.395 |
| guest narrow | ON | 57 | 52.004 | 29.682 | 21.882 |
| basic B | OFF | 62 | 47.982 | 27.716 | 19.877 |

ON 的平均源帧间隔比两侧 OFF 的算术均值长 **7.167%**，上一版宽标记轮为约 22.06%。这是两个不同轮次各自的短窗口观察，不是严格的宽／窄同包随机对照，也不是精确单标记成本；本轮仍不能把开启标记时的 29.682 ms Translate 当作无插桩基线。

全文件 Guest 范围记录为 267,296 条，其中完整帧窗口内 264,151 条。SVC 边界只有 `0x1C`（4127 次）和 `0x1D`（9585 次），没有 `0x0B` 或其他 SVC 的扩展 Guest 边界。基础 SVC tracing 未关闭。全文件记录不能与每帧开销直接线性换算。

### 真实消息通知链

窄标记得到 4054 次完整条件等待配对，排除 54 次越过完整帧窗口的等待及 23 次缺失／重复端点项。Translate 的真实线程为 **10640**，不是 header 的 present-marker 线程 10737。

Translate 线程的 **56 次**完整等待均由 **TID 10565** 选中通知：

| 阶段 | 平均 ms/次 |
|---|---:|
| 登记等待 → 选中通知 | 21.7987 |
| 选中通知 → wake | 0.0159 |
| wake → 返回（含虚拟核心重新获取） | 0.0497 |
| 完整等待 | 21.8642 |

等待边界仍为 `sdk+0x37eb3c`，返回线索 `sdk+0x32637c` 命中清单 vendor symbol `nn::os::ReceiveMessageQueue(unsigned long*, nn::os::MessageQueueType*)`；上层为 `main+0x126db3c`、`main+0x126cf74`。通知边界 `sdk+0x37eb44`，返回线索为 `main+0x126dc00`、`main+0x124ed74`、`main+0x1614c50`、`main+0x1614950`。这些 main 偏移仍未命名，不猜测具体游戏子系统。

SDK Build ID：`fcf93a41b08c5193a1410d262602dc621617c56f`；模块清单 SHA-256：`30513eed53bc4ae9bd1539c9e28dbffdb859b9c2c054fefd05aa9e0de3054f38`。逻辑 wait ID、epoch、waiter 和精确父 scope sequence 用于配对，不依赖邻近时间。FP 返回记录只是有界调用线索，不是完整 unwind。

等待仍主要发生在通知之前，不能由此断言生产线程持续计算、所有等待都可消除，或它们是在等 GPU。没有 scheduler/KGSL sidecar；这些是 elapsed，不是 on-CPU。各线程等待不可相加。

### 捕获完整性与边界残留

三份原始 PROF 的设备／本地 SHA、PID、模块身份及 capture ID 均已核验。当前安装包的公开 reader 独立验证均为 payload complete、truncated=false、interior scope_pairing_errors=0、chunks_skipped=0。

常驻旧 MCP 仍把后两份捕获起点残留的 unmatched ends 标为 truncated。窄标记有 29 个、basic B 有 34 个，全部在完整帧分析窗口之前，窗口内为零；仍保留 GPU orphan/incomplete-zone 和两端开放作用域诊断。表中 scope 时间只统计已闭合且与窗口相交的区间，不保证覆盖边界处未闭合的全部执行，更不是可相加的帧时间分解。

basic B 全文件还携带 **1797 条上一轮 Guest 记录**，时间为 `426285712384975..426285727490912`，早于本次完整帧窗口 `426295509525235..426298484394610`。汇总器最初因“OFF 文件应无任何 Guest 记录”的过强假设拒绝；随后按 SHA 绑定索引检查，确认 OFF 分析窗口内部 Guest 记录为零，才接受对照。没有删除残留、把它们算入 ON 成本或悄悄忽略 truncation。失败汇总和边界核验证据均保留。

## 数据与收尾

本轮已结束测试并停止自有进程 10383；未触碰另一台 Thor。临时 ordered/autoload 属性恢复原空值；Guest/detail/ring/capture 均关闭，准备缓存恢复开启。新 APK 保留在设备上，有序模式仍默认关闭。

结束审计：78 个原文件仍全部存在，无新增／删除；77 个哈希不变。唯一变化是 `option.sav` 的 4 字节，长度仍为 344；字段含义未验证，不能称为“只是计时”。前后字节均已保留，**没有恢复、覆盖或删除存档**。新 profiler 文件独立保留，不属于上述 78 文件集合。

没有 Root、锁频、清数据、卸载或删缓存。未提交、未推送。

## 证据位置

- 实现、构建和本地测试：`MacNativeGames/android/build/nvn-counter-report-20261027/`。
- 私有实机证据：`native-switch-study/android-translate-20261026/odin3-counter-report-20261027/`。
- `deployment.json`、安装前 APK、数据 tar 与安装前后 inventory：部署与保留证据。
- `off_a/on_a/on_b/off_b`：原始状态、日志、场景门槛记录、截图。
- `cadence-summary.json`：只统计有效窗口，以时长加权。
- `basic_a/guest_narrow/basic_b`：独立的 Guest 标记 OFF/ON/OFF 捕获，不混入无插桩 FPS。
- `preservation-final.json`、`changed-after-0.bin`：恢复诊断开关与数据变化审计。

采集脚本与私有数据分目录，读取私有文件的 Python 使用 `-I`。新的验证助手在拒收前也保留失败采样值，避免上一轮缺失触发门槛记录的问题。
