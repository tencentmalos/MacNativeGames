# Odin3 S1 第二轮：采样连续性已修复，开销验收未通过

日期：2026-10-28。接续 `SPEC_PIPELINE_PARALLELISM_20261028.md` 和首次 S1 失败记录。

## 结论

本轮不能宣布 S1 完成，也不进入 S2–S5 的行为优化。

- 完整 ABAB 的诊断关闭/开启合并值为 **32.7858 / 31.7849 FPS（−3.05%）**，未达到下降小于 2% 的门槛；四段存在明显基线漂移，不能把这个差值全归因于诊断。
- 更重要的是，完整 PROF 录制时 `Runtime.LaneSample` 自身达到 **17.41 / 18.12 ms/帧 elapsed**。逐事件序列化严重扰动了呈现通道，不能拿这两份 trace 判定原游戏的关键路径。
- 连续性修复有效：所有窗口均无丢弃 Present、队列溢出、未注册线程或 CPU 采样失败；调度锁忙时保留原始边界，稍后消费。
- 两份采样各选取连续 120 个完整源帧，工具已能重建显式 Guest 通知链。严格要求 enqueue 和 resume 都在 ROI 内时，两份均有 **119** 条 R←M 完整条件等待；另一个 enqueue 对应的 resume 在 ROI 外，不能冒充完整链。
- 按本阶段最多两轮“实现→ABAB”的约束，本轮停止继续修改运行时并写出证据；下一轮首先需要降低诊断数据量。

## 实现变化

`lane_event_queue.h` 使用有界事件环。Guest 调度器在现有 G 锁下写入原始时间、owner、wait 和退出事件，不在信号路径分配内存、调用 profiler 或读线程 CPU 时钟。

Present 消费者独占统计表，在 G 锁外读取 CPU 时钟和汇总事件；短暂 try-lock G 只取得发布水位。16 个待处理边界保留原始 CPU 样本，即使本次 try-lock 失败，后续仍按原始边界恢复统计。退出事件用于排除 TID 重用造成的 CPU 时钟身份污染。

诊断默认关闭；Mac Metal、Mac Vulkan、Android 共用该实现。虚拟核数量、mask、优先级和抢占规则均未改动，也未使用 `TOTK_FREE_CORES`。

`capture_pipeline.py` 修复 Android 15 的 SurfaceFlinger 图层名解析：保留 `(BLAST)#id`，去掉 `RequestedLayerState{... parentId=...}` 外壳。自动读档脚本已完成本轮启动，无需人工重复确认。

## 身份与配置

| 项目 | 值 |
|---|---|
| 设备 | Odin3 `6edb0593`，未操作 Thor |
| 包 / PID | `org.macnative.android.botw` / `29736` |
| APK SHA-256 | `4bb8e864532183fefbc65e158be09cec94ada7156498c29d454ea66d26838335` |
| 平台 ELF Build ID | `fdafeed349a67d7d6c4b3ec56dd1b1858d158eb9` |
| BOTW main Build ID | `8e9978d50bdd20b4c8395a106c27ffde`（尾部零填充规范化） |
| Turnip SHA-256 | `e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`，未换驱动 |
| 基线 | publication_queue=1、invalidate_fast=1、ordered_translation=0 |
| 其他 | 720p 手持、internal scale 1、realtime=1、PSO workers 4、60 Hz |

场景由 latest-save 自动加载、draw/dispatch/暖 shader/新鲜 Present 门槛确认；同进程期间未注入移动输入。地图和精确坐标没有独立解码，不能把脚本门槛当作坐标证明。

## 验证与开销

Mac 构建通过，porting 26/26、profiler 2/2；accounting 与事件环 UBSan 通过。新增事件环测试覆盖队满显式失败、延迟边界恢复和 100000 次并发环绕。Android assemble/lint、真实 SDK 字符串契约测试通过。Mac MoltenVK 与 Odin3 Turnip probe 均重新链接，OFF/ON/ordered 六种组合全部通过。

| 窗口 | 成功 native Present FPS | 时间 s | 热状态（前/后） |
|---|---:|---:|---|
| OFF_A | 29.5142 | 30.3583 | 0 / 0 |
| ON_A | 29.5406 | 30.4666 | 0 / 0 |
| OFF_B | 36.0463 | 30.4608 | 0 / 0 |
| ON_B | 34.0287 | 30.4742 | 0 / 0 |

这些窗口不录制 PROF。合并按 Present 总数 / 总时间计算。OFF_A→OFF_B 的漂移尚未解释，热状态相同不能证明频率、温度或后台负载完全一致。诊断模式本身未通过门槛，不做第三轮调参。

## 不录制 PROF 时的通道统计

以下取 ON 窗口前后快照差值，排除初始化前缀，分别覆盖 906 / 1042 个完成边界；分母与轮询 FPS 的取样边界不同。

| 指标 | ON_A | ON_B |
|---|---:|---:|
| 虚拟核 0 持核比例 | 37.04% | 39.19% |
| 虚拟核 1 持核比例 | 43.36% | 46.77% |
| 虚拟核 2 持核比例 | 87.56% | 94.09% |
| R `0x20004` 持核 ms/帧 | 0.109 | 0.123 |
| R 线程 CPU ms/帧（全部工作） | 24.683 | 22.099 |
| R 等虚拟核 ms/帧 | 0.441 | 0.564 |
| worker `0x58009` 等虚拟核 ms/帧 | 0.839 | 0.777 |
| worker `0x6000a` 等虚拟核 ms/帧 | 1.250 | 0.990 |

三核平均持核约 56.0% / 60.0%，不是三核共同饱和。持核比例也不等于 on-CPU 比例。核 2 的高占用需要进一步区分有效工作与高频让出/抢占；不能据此扩大 NCORE。

R 的绝大部分 CPU 工作已在虚拟核之外，与 Submit 的 HostWait 实现一致。因此 S2 的价值是解除 R 的同步返回依赖，不是再把这 22–25 ms 从虚拟核预算中扣一遍。

## 连续采样、扰动与证据限制

| 指标（每份 120 个完整源帧） | capture-a | capture-b |
|---|---:|---:|
| SDK 帧范围 | 471–590 | 664–783 |
| ROI 时长 ms | 4972.738 | 4943.307 |
| NVN.Translate elapsed ms/帧 | 29.345 | 29.243 |
| WorkerMgr::sync 累计 elapsed ms/帧 | 21.487 | 21.122 |
| Runtime.LaneSample elapsed ms/帧 | 17.412 | 18.119 |
| R←M 完整条件等待 elapsed ms/源帧 | 9.732 | 9.204 |
| PROF 中帧间隔 p50 / p90 / p99 ms（最近秩） | 40.815 / 51.691 / 63.814 | 41.224 / 50.612 / 59.140 |

父子 scope 和跨线程 elapsed 不能相加。capture-a 的 `Runtime.LaneEvent` 有 171930 次，即约 1433 次/帧，累计 self elapsed 16.594 ms/帧；CPU 时钟读取本身只有约 28 µs/帧。昂贵部分是将每个调度事件展开成 scope 加多个 counter，而不是读取 CPU 时钟。这里的 self elapsed 仍包含宿主抢占，不是 on-CPU 时间。

两份 PROF 容器完整、无跳过 chunk、无 scope pairing error；保留了首尾未闭合 scope、GPU orphan/incomplete zone。未证明 GPU/CPU 全局时钟校准，因此不推导 GPU 空闲率或 CPU→GPU 绝对延迟。录制间遗留事件造成全文件时间跨度远大于 8 秒，已通过内部连续源帧 ROI 排除，不使用全文件首尾计算 FPS。

Perfetto 成功取得 sched 和 CPU frequency。整个 10 秒捕获内，R 的已知频率 on-CPU 加权值约 2.766 / 2.779 GHz；这是录制诊断时的 R 整线程结果，不能当作未扰动的翻译频率。解析器还报告 15022 / 15016 个 `systrace_parse_failure`，必须保留该限制；未把这项警告解释为零成本或完整 atrace。

SurfaceFlinger 读取到 60 Hz 时间戳。含保留前缀的去重历史中，A 的 1/2/3/4 间隔计数为 7/218/83/11，B 为 56/189/92/3。它们不是严格裁剪到上述 PROF ROI 的分布，不用来宣称 S5 的收益；没有显示帧身份时也不能称差额为游戏丢帧。

## Q1–Q6 与 D2

| 问题 | 本轮结论 |
|---|---|
| Q1 M/R 是否锁步 | R 等 M 的显式通知已闭合；反向完整依赖和未扰动关键链占比未闭合。H2 未判定。 |
| Q2 M 等哪些 worker | 地址通知证明确有 `0x58009`、`0x6000a`，还有超时轮询及其他条件通知。不能把最终一次短等待等同整段 WorkerMgr::sync，也未给所有 worker 建立同步的 on-CPU/持核区间交集。 |
| Q3 三核占用 | 非录制窗口见上表，核 2 热、核 0/1 相对轻；不支持“3 核共同达到上限”的说法。 |
| Q4 持核但未运行 | CPU 边界采样只提供上下界；last_svc/last_hle 是最后记录值，不是整个区间的当前调用。未据此添加 core_away。 |
| Q5 R 的核、频率、runnable | Perfetto 有证据，R 会迁移且约 2.77 GHz；捕获扰动和精确翻译区间关联尚未解决，暂不做亲和性/调频结论。 |
| Q6 上屏间隔 | 已采到有效图层数据，但严格 ROI 对齐与无扰动分布仍未验收。 |

**D2：保持在 S1。** 下一轮先设计低数据量 Lane 输出：保留原始事件时间、稳定线程身份和代次，以紧凑记录或有界独立 sidecar 批量输出；不要继续逐事件发送八个 counter。普通 Present 汇总和高频 owner 明细应独立开关。先测完整录制的开销，再补显式 Submit/Present/fence 身份连接，之后才选择 S2/S3/S4/S5。不得通过关掉虚拟核约束取得“上限”数字。

S0 的 identity 台账和生命周期矩阵仍待闭合；publication/invalidate 源码默认值仍为 0，没有应用改默认值 diff。

## Litep 工具修复

按用户授权，在 `/Users/bytedance/workspace/my_mcp_tools` 的 `codex/litep-native-guest-waits` 分支增加 native Guest 等待适配。原通用工具只接受 `GuestSync.*`，对 `Guest.Condition/Address.*` 返回了 0 条链，这不是没有等待。

新适配按 scope 的 parent/zone sequence 组包，再按 domain + epoch + WaitId + waiter 关联。Guest handle 与物理 TID 分开；Condition 没有 object 地址就保留 unknown，Resume 不冒充 mutex reacquire；重复通知、混合字段、丢失端点和超时都明确输出。逐帧贡献工具也复用这份适配。

66 项相关测试通过，包括旧 GuestSync、native 包、跨帧关系和 MCP 服务。通过开发版公开 MCP 的 initialize/tools/list/open_trace_index/analyze_wait_chains 对两份真实索引复算，分别得到 10932 / 11017 个 ROI 内完整等待；R←M 的数量和耗时与独立公开 SQL 核对一致。当前为源码和开发 MCP 验证，未切换全局安装版本。

## 收尾与证据

证据目录：`/Users/bytedance/workspace/native-switch-study/android-translate-20261026/pipeline-s1-retry-20261028`。

- `abab/`、`lane-cadence-summary.json`：四段独立帧率和不录制时的通道统计。
- `capture-a/`、`capture-b/`：原始 PROF、Perfetto、SF、ROI、显式链、开发工具结果及完整性记录。
- `tool-provenance.json`、`litep-tool-tests.log`：工具源文件哈希、分支及测试。
- 用户授权的 ptracy 可读目录通过本轮自有 MCP 客户端 roots/list 提供，没有把原始证据复制进 Git。
- 安装前后 78 文件哈希一致；运行结束仍为 78 文件，仅 `option.sav` 被游戏正常改写，已保留，无删除和回滚。
- 两次采集 finally 均关闭采样，8 个启动属性恢复原值。BOTW 随后于 16:28:05 `EXIT_SELF/status=0` 正常退出；未把其他包的后续 crash 日志当作 BOTW 故障，也未重新抢占设备前台。
- MacNativeGames 与工具仓库的本轮改动均未提交、未推送。
