# Android Profiler O2 修复与 S1 复测

## 结论

**O2 修复通过，S1 的低扰动门槛通过。** Android Debug 的 Foundation ring、
profiler SDK、LZ4 共20个编译对象现在最终使用 `-O2`，保留调试信息和断言。
同进程通道计数 OFF/ON 合并为 **29.4828 / 29.4680 FPS（−0.050%）**。
完整录制的两份连续120帧为29.393 / 29.480 FPS，通道发射耗时约0.50 ms/帧。
上一轮约5.4 ms/帧的记录成本已消除大部分，但这不是未录制游戏吞吐提高的证明。

新的两次独立采样均指向 R 上的同步翻译：约23.9 ms elapsed、23.6 ms线程CPU。
翻译期间宿主 runnable 仅0.052 / 0.104 ms/帧，实际频率2.652 / 2.626 GHz。
**D2 选择先做 S5 ADPF 实验**，S3 的 Draw/预检降本保留；没有证明 M↔R 双向锁步，
不以这份数据放宽 S2 准入或 Guest 调度规则。

## 改动与验证

- 编译策略位于共享 `porting/profiler/CMakeLists.txt`，不编辑 Foundation。
  Debug 为优化构建，其他构建类型保留原优化级别；Mac 当前 RelWithDebInfo 原本已为O2。
- 新增真实SDK发射微基准：每个合成周期1400条紧凑核事件，OFF/ring/file，
  O0/O2/O0/O2，各模式960个测量周期。ring CPU成本671.24→93.34 µs，
  file 720.65→100.78 µs，均约降低86%；这是微基准，不能换算为游戏FPS。
- 修复SDK测试夹具：冷启动路由需第一个实际FrameMark后才能记录；此前的
  interner测试会空跑。现在显式检查Enabled，Mac/Android实际记录状态下通过。
  微基准早期另有空 `error=` 状态误判，已保留失败输出并修正，均非游戏崩溃。
- A/B脚本增加120秒无录制恢复、末60秒预声明稳定性门槛；所有A/B窗口完成后再录制。
  自动读档的busy重试采用有上限的退避，本次未请求用户手动进场景。
- O2版本：27项底座测试、3项profiler测试，双平台OFF/ON/ordered六组Vulkan probe，
  Android构建和lint通过。后续ADPF版本的测试与收益单独记录。

## 身份与配置

- Odin3 `6edb0593`；包 `org.macnative.android.botw`；游戏PID `15455`。
- APK SHA-256：`3e369b73a56ae792f28624333ac9ffdfdf93412f318e8aebd464526dae892f85`。
- ELF Build ID：`e5b270e9b17e548be474529a465113947a3c0825`。
- BOTW 1.6.0 main：`8e9978d50bdd20b4c8395a106c27ffde`。
- Turnip：`e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`。
- 720p手持、内部1×、realtime=1、PSO workers=4、60Hz；启动显式
  publication=1、invalidate=1、ordered=0，源码默认值未改。
- 自动读取最新存档，Draw≥1000、Dispatch存在、无新建shader/PSO、连续呈现门槛通过。
  本轮没有注入跑图输入；精确地图/角色姿态没有新的语义校验，不能宣称逐像素同场景。

## 未录制 A/B

| 窗口 | 成功native Present | 秒 | FPS |
|---|---:|---:|---:|
| OFF_A |905|30.6431|29.5336|
| ON_A |904|30.6855|29.4602|
| OFF_B |901|30.6130|29.4319|
| ON_B |904|30.6691|29.4759|

恢复窗口122.51秒、29.4266 FPS，稳定性门槛通过。四窗无PID、前台或配置漂移。
通道计数开销约0.05%；差异低于2%，不主张游戏性能提升。

## 两次独立 Litep + Perfetto + SurfaceFlinger

每份取排除前20个完整帧后的连续120个SDK源帧，拒绝超过200ms的跨采集桥接区间。
不同线程与嵌套scope不可相加；scope为elapsed，线程CPU与调度事件另列。

| 指标 | A | B |
|---|---:|---:|
| native FPS |29.393|29.480|
| native p50/p90/p99，ms |33.331 / 37.817 / 47.736|33.352 / 37.823 / 47.647|
| 实际上屏FPS |29.388|29.507|
| SF 1/2/3个vsync间隔数 |3 / 108 / 8|2 / 111 / 6|
| R Translate elapsed，ms/源帧 |23.900|23.911|
| R Translate线程CPU，ms/源帧 |23.606|23.548|
| R Draw elapsed，ms/源帧 |17.659|17.555|
| R CpuRead.Preflight，ms/源帧 |3.543|3.582|
| R Publication.Drain，ms/源帧 |0.602|0.836|
| R Condition.Wait，ms/源帧 |8.625|8.197|
| M WorkerMgr.sync，ms/源帧 |17.526|17.236|
| M postCalc，ms/源帧 |16.017|16.121|
| M Address.Wait，ms/源帧 |9.060|9.240|
| 呈现CopyFence，ms/源帧 |1.437|1.501|
| LaneSample，ms/源帧 |0.501|0.496|
| 翻译期间加权频率，GHz |2.652|2.626|
| 翻译期间runnable，ms/源帧 |0.052|0.104|

CPU/调度对齐用LaneFrame scope包围的BOOTTIME采样以及MONOTONIC前后锚点，
保留转换余量；A的offset不确定度约±21.4 µs。没有建立GPU/CPU查询时钟对齐。
SF按独立时间窗口去重、去除pending fence，未假造逐个Guest/native/显示帧对应关系。

## S1 六个问题

1. **Q1：H2未证明。** R=`0x20004`/TID15795，M=`0x8001`/TID15628。
   显式enqueue→selected-notify→resume证明R等待M约8.621 / 8.192 ms/源帧；
   未观察到M等待R的完整反向通知链。不能从WorkerMgr.sync与Translate重叠推断锁步。
2. **Q2：已识别worker，完整任务链仍缺。** M被`0x58009`/TID15802通知的
   address等待约1.088 / 1.036 ms/源帧，被`0x6000a`/TID15820通知约0.577 / 0.640 ms。
   另有7.366 / 7.531 ms超时完成、无通知者，不能补造worker归属。
   尚未把每个WorkerMgr子任务及其持核区间完整绑定到调度状态，因此不按S4修改阻塞点。
3. **Q3：三个虚拟核整体没有饱和。** 未录制通道计数区间（含窗口前后查询）的占用分别为
   32.75/39.40/79.59%和32.69/40.27/80.26%，平均约51%；核2最热。
   完整录制中约40/45/90%，说明即使FPS接近，细粒度调度仍会受到插桩影响。
   `0x58009`等核约0.64–0.67 ms/帧，`0x6000a`约0.88–0.92 ms/帧。
4. **Q4：只能给持核离CPU的界，不能归因某个HLE。** 未录制时`0x22002f`
   下界1.19–1.29 ms/帧，上界约10 ms；`0x58009`下界0.72–0.75 ms，
   `0x6000a`下界约0.68 ms。末次SVC字段不是当前阻塞调用，last_hle=0不构成证据。
   这些宽界不足以证明“持核阻塞在某宿主API”，保留未知项。
5. **Q5：R主要在执行，频率值得实验。** 翻译调度running分别2860.20/2854.85 ms，
   runnable总量6.26/12.50 ms，sleep 1.55/1.97 ms（各120帧）；运行横跨CPU0–7，
   采到的频率最高3.072GHz。选择有界ADPF实验，不绑Guest宿主核、不锁频。
6. **Q6：主要是两个60Hz间隔，少量三个间隔。** 表中SF时间戳证实实际约29.4FPS。
   120Hz只能影响量化，不先把它当作翻译吞吐修复。

S1的低扰动与两次通道排序已有证据；逐任务完整关键路径比例仍未闭合，不把S1所有项标为完成。

## 完整性、保全与后续

A PROF SHA为`cea17f7786763381d4e66a319ffd6c6fd139f682334039c631ff8b01b43badf7`，
B为`09ec65cee9cb34140110e1c3839a900eab65578c28af4641c0a0112e0ceb7ffe`。
1288/1285个chunk，skipped=0、truncated=false、scope_pairing_errors=0；
保留边界未闭合scope、不完整GPU zone及2/4个orphan GPU time诊断。
核事件包校验无错误、没有未知owner，不把GPU尾部缺失当成零等待。

安装前后78个文件哈希相等；运行后仍78个，只改变游戏正常写入的`option.sav`，
不回滚。启动属性恢复、采样结束均关闭。进程在采集结束后18:29:18正常退出，
系统记录EXIT_SELF/status0，前台已变为shadPS4启动页；本轮未向其他应用发送停止命令。

证据根：`/Users/bytedance/workspace/native-switch-study/android-translate-20261026/pipeline-profiler-o2-20261028`。
`abab/`为诊断ABAB；`capture-a/`、`capture-b/`为两份联合采集；SQL、公开Litep
查询结果、时钟校准、调度/SF分析均在同目录。原始PROF和用户数据不入Git。
阶段diff：`android/build/pipeline-profiler-o2-20261028/change.diff`。
ADPF设计见[下一阶段设计](ODIN3_ADPF_DESIGN_20261028.md)，
[实机结果](ODIN3_ADPF_20261028.md)为+1.942%、长窗口中断，未通过验收，保持默认关闭。
