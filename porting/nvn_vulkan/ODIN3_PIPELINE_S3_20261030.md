# S3：Draw 与范围预检诊断检查点

整理日期：2026-10-30。采样尝试与证据目录沿用 2026-10-28。
依据：[流水线规范 v3](SPEC_PIPELINE_PARALLELISM_20261028.md) 的 S3，以及
[发布解耦规范](SPEC_PUBLICATION_DECOUPLING_20261027.md) 的 W3。

## 当前结论

本检查点补齐共享 Vulkan 的细分诊断，尚未完成 S3 热点排名、前两项优化或性能验收。
Mac 与 Android 使用同一份适配脚本与引擎实现，没有新增平台优化分支。
`publication_queue`、`invalidate_fast` 和 ADPF 的源码默认值不变。

此前 O2 基线中的 Draw 约 17.6 ms、预检约 3.56 ms 是粗粒度参考，不能据此宣称已找到
两个具体子项，更不能将本次诊断提交报告为性能提升。

## 诊断边界

原先 `CpuDetailDrawScope` 在范围预检之后才选择抽样，导致预检细项不受同一次 Draw 的
1/N 决策约束。现在在 Draw 入口选择，`profiler_detail start 32` 下整个调用共用该决定。
`NVN.Draw.CallSample` 包围所选调用；原有 `NVN.Draw.Sample` 保持原来的 Draw 内部边界。
早返回调用仍能保留外层边界，分析时必须分别报告调用数和实际进入 Draw 的数量。

| 区域 | 新增或细分的标记 |
|---|---|
| Descriptor | `NVN.Draw.Descriptors`、`NVN.Descriptor.Key`、`.Memo`、`.Build`、`.Dynamic`、`.Images` |
| Snapshot | `NVN.Snapshot.Uniform`、`.Vertex`、`.Index`、`.Storage`、`.Other` |
| 顶点与索引准备 | `NVN.Draw.VertexIndex` |
| 范围预检 | `NVN.CpuRead.Plan`、`.Bindings`、`.Storage`、`.VertexIndex`、`.Targets`、`.RangeCheck` |
| 抽样工作量 | `NVN.CpuRead.SampledBindingVisits`、`.SampledRangeVisits` |

未更改 snapshot、descriptor、范围检查、资源失效或退休规则。原有 CPU timing 分类保留，
Snapshot 通过重命名现有 scope 细分，避免添加一层重复计时。
Compute 等 Draw 外的调用沿用原 detail 行为，不能混入 1/32 Draw 的样本分母。
统计父子区域时应使用裁剪后的 exclusive 区间或区间并集，禁止相加 inclusive 时间。

## Odin3 采样尝试

- 设备 `6edb0593`，应用 `org.macnative.android.botw:game`，PID `28251`。
- 复用 O2/ADPF 检查点 APK；自动读取最新存档的场景门禁成功。
- 配置为手持 720p、1×、realtime=1、PSO workers=4、显示 60 Hz，显式启用
  publication/invalidate；ordered 和 ADPF 关闭。
- 121 次恢复采样覆盖约 142.41 s，恢复稳定性检查失败；四个正式采样窗口均未开始。
  后续观察到前台为 shadPS4，BOTW 以 `EXIT_SELF` 退出。无法据此确定恢复阶段波动的原因。
- 本轮有效 PROF 数量为 0，优化实现→ABAB 轮数为 0。失败前缀已保留。
- 设备独占确认尚待回复，诊断 APK 未安装，Android GPU 探针尚未执行。

## 离线验证

- Android native 探针及 APK 构建通过，`lintBotwDebug` 通过。
- Mac Metal/Vulkan 构建通过；共享底座 CTest 28/28、profiler CTest 3/3、
  porting Python 测试 33/33 通过。
- Mac MoltenVK GPU 探针 OFF、ON、ordered 三种模式全部通过。
- 真实 SDK 测试覆盖 stride=32 时 96 次外层 Draw 恰好选择 3 次；嵌套调用继承选择，
  不推进外层计数；停止 detail 后不再选择。
- 合入前完整 `make check` 通过：342 项 Python 测试、27 个 RomFS 路径用例和手柄布局用例。
  使用仓库 requirements 的独立 Python 3.14 环境及显式 Xcode 26；没有修改全局工具链。

诊断 APK SHA-256：
`2c8d6811f3580bb72a7f1f12643cbf70d282fcc5cc1aba23923100d5c7c8774e`。
构建通过不等于 Android 场景正确性或性能验收通过。

## 继续执行

设备可独占后，先运行 Android 三模式 GPU 探针，再原位安装诊断 APK，自动读取同一存档。
在同一进程采集 detail OFF/32/OFF/32，相邻 OFF 窗口量化抽样扰动；选取连续完整源帧，
按 `CallSample` 层级区分预检与 Draw，按 exclusive 开销排序。取得两份一致的排名后，
只处理前两项，再执行未录制 ABAB、Litep 线程 CPU 与调度频率对照。
验收仍为 Translate 线程 CPU 至少下降 8%、FPS 至少提升 2%，并保留 p90/p99、
范围/别名/同地址内容变化回归以及 GPU 读回证据。

## 证据位置

原始日志、失败前缀和构建产物身份保存在仓库外：
`/Users/bytedance/workspace/native-switch-study/android-translate-20261026/pipeline-s3-20261028`。
关键入口为 `baseline/result.json`、`baseline/recovery.jsonl`、`scene/result.json`、
`diagnostic-apk.json`、`mac-probes.json` 与 `integration-20261030/` 中的门禁日志。
游戏数据、APK、PROF 与生成的引擎源码不进入 Git。
