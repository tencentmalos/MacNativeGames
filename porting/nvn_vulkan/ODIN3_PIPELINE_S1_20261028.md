# Odin3 流水线诊断 S1：首次实机失败与恢复记录

日期：2026-10-28。依据 `SPEC_PIPELINE_PARALLELISM_20261028.md`。

## 结论

**S1 未通过，D2 暂不选择优化分支。** 本轮只增加诊断，没有改变虚拟核规则、翻译执行方式或 Vulkan 优化默认值。
首次 Litep 录制触发新增诊断代码的字符串生命周期断言。按 v3 的通用停止条件及 v2 §4，停止实机实验，保留现场并恢复 W2 已验证 APK。
根因已在源码离线修正，实际 SDK 回归通过；修正版未安装、未进行第二次 ABAB，不能宣布性能改善或 S1 验收完成。

## 已实现

- `lane_status on|off|status|reset`，默认关闭。Mac Metal、Mac Vulkan 与 Android 共用 `threads.c` 的调度观测与 debugbus 回调。
- 按真实虚拟核所有权转移计持核、空闲、各线程持核时间；等待按 waiter 及入队时观察到的 owner 集合计数。使用独立 thread id，避免句柄复用串账；256 个线程、512 组等待关系的固定容量与显式溢出计数。
- 每个成功 Present 尝试一次 CPU 时钟采样；未获取调度锁则记录 skipped，不阻塞呈现。输出线程 CPU、持核 CPU/非 CPU 的上下界、CPU 未覆盖时间、last_svc，以及明确标记为“最后被追踪调用”的 last_hle。
- 不把线程总 CPU 时间直接当作持核期间 CPU。R 在 `HostWait` 让出虚拟核后仍执行翻译，两者无法从边界样本精确分离。界限计算考虑采样时间偏差，精确归属仍需调度与所有权轨迹。
- 持续区间在 Present 截断；status 使用最后完成边界的快照，不混入尚未采 CPU 的尾段。查询使用 try_lock，格式化和 Litep 发射在调度锁外。
- 测试脚本新增 lane 模式 ABAB、8 秒 Litep 录制与非 root Perfetto/SF sidecar 脚本。后者因停止条件尚未实机验收，不能宣称调度或上屏证据已经齐全。

## 精确身份

| 项目 | 值 |
|---|---|
| 设备 / 包 | Odin3 `6edb0593` / `org.macnative.android.botw` |
| 实验 PID / 崩溃 TID | `17873` / `18152` (`mng-present`) |
| main Build ID | `8e9978d50bdd20b4c8395a106c27ffde`（尾部零填充保持于原始证据） |
| 失败 APK SHA-256 | `673c7acedc9a1442ed2235d34577ad1e37c2e45c73e5bfe11972005fcb612453` |
| 失败 libmacnative_platform.so Build ID | `0d761a70f1f2e37a6a2c47566ed33bc5f74e3f12` |
| 恢复 APK SHA-256 | `28a93251c4e50cc3cee4007f7c38806e2945bb0922ef37e07b9b9605a3020917` |
| Turnip SHA-256 | `e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d` |
| 配置 | publication=1、invalidate_fast=1、ordered=0、720p handheld、internal 1×、realtime=1、PSO workers=4 |

按原存档自动加载并通过 ≥1000 draws、有 dispatch、shader/PSO 已热身、持续新 Present 的场景门槛。
未注入移动输入；没有证据证明此次视角与历史 W2 完全相同，不能跨报告直接比较 FPS。

## 失败与证据

第一次启动 PID 16825 在加载期间发生 Activity CLOSE，系统记录 `EXIT_SELF 0`，不作为性能样本，也不称为 native crash。
重新启动 PID 17873 后进入场景并开始 ABAB。只完成了前两段：

| 段 | 成功 native Present | 秒 | FPS |
|---|---:|---:|---:|
| OFF_A | 1057 | 30.385499 | 34.7863 |
| ON_A | 894 | 30.424917 | 29.3838 |

**这是未完成的 ABAB；不能把差值作为诊断开销定论，更不能作为优化收益。** 两段 Thermal Status 均为 0，但没有频率和调度轨迹解释差异。
ON_A 之后的状态累计记录：947 次成功采样、103 次 skipped（占尝试次数约 9.81%），row/pair overflow=0、CPU failures=0。
CPU 采样区累计 53.808 ms，平均约 56.8 µs/成功采样，最大 212.8 µs；该数值不包括全部复制和录制开销，也不能解释整体 FPS 差异。
采样不连续本身已不足以满足 S1 的逐帧完整性要求。raw lane snapshot 遇到 busy 时原脚本也没有重试；现已离线补上有界重试，原 busy 证据未覆盖。

2026-10-28 15:33:18，首次开启 Litep 后触发 SIGABRT。使用与崩溃日志一致 Build ID 的精确 ELF 符号化：

```text
StringIntern::check_static_string_contract
  -> spatial::LiteTrace::counter
  -> runtime_lane_present (lane_runtime.inc:108，失败版源码)
  -> mng::diagnostics::frame
  -> mng_present_wait
  -> SerialHandoff::run
```

新增代码曾用可复用栈缓冲拼接 Counter 名称。SDK 以指针身份缓存静态字符串，后续更改同地址内容违反契约。
这是本轮诊断实现错误，不是 Vulkan GPU 故障。原始 partial PROF 没有完成 footer，不用于帧率、等待链或 GPU 性能结论。
源码修正为固定字符串字面量；handle、thread id 和 TID 作为 `Runtime.LaneRow` 内的数值字段。真实 SDK 回归启用 ring，覆盖多核、多行、重复帧和变化的线程身份，避免 mock 掩盖同类错误。

## 检查与恢复

- 初版 Mac Metal/Vulkan 构建、Android assemble/lint 通过；porting 25/25、profiler 2/2；lane accounting UBSan 通过。
- 初版显式重链 probe 后，Mac MoltenVK 和 Odin3 Turnip 的 OFF、ON、ordered 共 6 次通过，包括既有 GPU 时钟、fence、像素与对象生命周期校验。
- 离线字符串修正后 Mac Metal/Vulkan 与 Android assemble/lint 再通过；含真实 SDK 新用例的 profiler 2/2 通过。没有再次做实机 ABAB。
- 离线修正 APK SHA-256：`b1ca71e81dc35b3b882699d3894dddf3d9d31bbca790004baa82b2991043ae0a`；保留为 `app-botw-s1-offline-repair-not-installed.apk`，**没有安装**。
- Odin3 已恢复 W2 APK；安装和恢复分别逐项比对 78 个用户数据文件，安装操作均未改变它们。整个运行期仅 `option.sav` 被游戏正常改写；未回滚这个文件。
- 恢复后 PID 21963 自动进入原存档并通过场景门槛。恢复检查短窗为 151 presents / 5.110466 s（29.55 FPS），仅证明恢复后的连续呈现，不是新的性能 A/B。
- 所有采样关闭，启动属性恢复原值；当前存活进程的 publication/invalidate 保持 1/1。源码默认值仍为 0；下一次重启需测试脚本显式设置。
- 未 root、锁频、换驱动、清数据、卸载、提交或推送；未操作 Thor。

## S1 问题及 D2

| 问题 | 当前结论 |
|---|---|
| Q1 M/R 是否锁步 | 未定；缺少两份连续 ≥120 帧显式等待链 |
| Q2 M 等哪些 worker，worker 在做什么 | 未定；缺少选中通知及调度关联 |
| Q3 三个虚拟核占用与等待 | 接口已实现，采样存在 skipped，不能作为完整逐帧验收 |
| Q4 持核却未运行的 HLE/SVC | 只实现有误差界限的边界统计；无完整所有权/调度交集，不能归因 |
| Q5 R 的 CPU 核、频率、runnable | Perfetto 尚未采集；保持未知 |
| Q6 上屏 vsync 间隔 | SF sidecar 尚未采集；保持未知 |

**D2：继续停留在 S1，不进入 S2–S5。** 下一轮先验证修复后的真实 Litep 发射，再解决 skipped 和诊断开销；不能通过等待调度锁把观测本身变成新的串行瓶颈。
需要把边界采样请求与调度锁内的轻量账本切换解耦，并明确异步切换的时间误差；结合可用调度轨迹决定是否补所有权事件。CPU 时钟不能移到每次 take/put。
随后完成同进程完整 ABAB、两份 ≥120 帧 Litep、10 秒非 root sched/frequency 和 SF 证据，再选择 D2。
S0 的 identity 台账、生命周期矩阵及未应用的默认值 diff 仍待完成；本轮未修改默认值。

## 证据位置

仓库外：`/Users/bytedance/workspace/native-switch-study/android-translate-20261026/pipeline-s1-20261028`。

- `ab/result.json`、`ab/*.jsonl`：失败的前两段与 cleanup 错误。
- `ab-native.txt`、`ab-exit-info.txt`、`ab-exit-system.txt`、`crash-symbolized.txt`：崩溃、系统退出记录和精确栈。
- `symbols/`、`failed-source/`、`failed-prof.json`、`*.prof.partial`：原始失败版本证据。
- `mac-probes.json`、`android-probes.json`、`offline-repair-*.log`：验证与离线修复检查。
- `deployment/`、`rollback.json`、`rollback-files-*.json`、`restored-scene/`、`final-runtime.json`：数据保护和恢复。
- 本阶段差异：`android/build/pipeline-s1-20261028/change.diff`（也复制在证据目录）。不包含此前未提交改动。
