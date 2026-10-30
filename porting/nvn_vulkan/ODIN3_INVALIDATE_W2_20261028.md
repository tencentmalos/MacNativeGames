# Odin3 W2：范围 Invalidate 解除前端锁等待（2026-10-28）

## 结论

本阶段的同进程性能门槛通过：固定 `publication_queue=1`，只切换 `invalidate_fast`，
合并 **28.4254 → 29.3866 FPS（+3.381%）**，两对 ON 均高于相邻 OFF。
M/P、两个主要 worker 和 R 在 ON 的完整源帧窗口里均没有 `NVN.FrontLock.Wait`；
R 的消息条件等待下降，翻译 CPU 时间基本不变。符合 D1 的“等待转移到另一个 API，逐项处理”分支。

这不等于 CPU/GPU 已经饱和，也不关闭 W1 尚未完成的 I2/I5 全矩阵验收。
W1/W2 的源码默认均保持 0，没有改默认、提交或推送。本次在途进程保留两个优化开启供观察。
下一阶段应按 spec W3 测量并降低 Draw / read preflight 的实际 CPU 成本，不扩大 Flush 放行范围。

## 实现与边界

- `runtime_policy.h` 增加 `invalidate_fast / MNG_INVALIDATE_FAST=0`。Mac MoltenVK / Android Turnip 共享实现；Android 只传递显式属性。
- `nvnMemoryPoolInvalidateMappedRange` 在独立 `flush_dependencies` 上查询范围。没有 CPU 输出交集，或仅有 Image 权威状态时，执行 acquire fence，不获取 front/engine 锁。
- Writeback、Report、Event、Unknown 以及未来未知 kind 保留原路径；物理/虚拟别名、有序翻译、范围 Invalidate 关闭也保守回退。不改 Flush、Fence、纹理引用或回收。
- `renderer_invalidate status|on|off` 不等待渲染锁；请求值 `invalidate_fast` 与门控后的 `invalidate_effective` 分开报告。原 `renderer_schedule invalidate off` 同步关闭快速路径资格。
- 对照 Mac Metal 的 `mtl_pool_invalidate`：先查 GPU 写回交集，存在交集才进行等待和写回。没有修改 Metal 或 D2R 源码。
- `benchmark_publication.py --mode invalidate` 固定发布队列开启，保留四段未采样 FPS、各自 3 秒 Litep、热状态和非 root CPU ticks。
- `validate_scene.py` 在确认没有游戏进程时重建启动页 task，确保 `test-launch` 由 `onCreate` 处理。应用数据不清除；已有进程仍要求显式 `--restart`，`--existing` 不重启。

## 身份和配置

- 设备：Odin3 `6edb0593` / Adreno 830；未操作 Thor、未 root、未锁频、未换驱动。
- APK SHA-256：`28a93251c4e50cc3cee4007f7c38806e2945bb0922ef37e07b9b9605a3020917`。
- 完整 A/B PID：`10583`；M/P handle `0x8001` → TID `10709`，R `0x20004` → `10786`，worker `0x58009` → `10792`，`0x6000a` → `10793`。
- live main Build ID：`8e9978d50bdd20b4c8395a106c27ffde00000000000000000000000000000000`；通过 `guest_modules` 验证，不以启动 manifest 代替。
- live Turnip SHA：`e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`，保留实际 `/proc/10583/maps` 与映射文件哈希。
- ELF Build ID：`libmacnative_platform.so=ba108208d03048832ad83d07c20da44fd9ce575f`；probe `bb7169b8548ec88f17b5d15d8c9908a1cc0cb643`；符号文件另存 Git 外。
- BOTW 1.6.0，720p 手持 / internal 1× / realtime=1；ordered=0、image_flush=1、live_range_flush=1、range_invalidate=1、preparation_plan=1、actual PSO workers=4、batch=1024。完整稳定字段见 `ab2/result.json`，无配置差异。
- 自动加载设备原存档，要求保存加载完成、≥1000 Draw、有 Dispatch、shader/pipeline 新建为 0、持续新 Present，再稳定 15 秒。原存档用于复苏神庙验证；本轮没有独立识别地图或固定姿态，不将 workload gate 冒充画面检查。

## 同进程 ABAB（未录制窗口）

| 窗口 | 成功 Present | 秒数 | FPS | 进程占全 CPU ticks |
| --- | ---: | ---: | ---: | ---: |
| off_a | 867 | 30.479262754 | 28.4456 | 29.560% |
| on_a | 895 | 30.514790231 | 29.3300 | 30.314% |
| off_b | 863 | 30.381780942 | 28.4052 | 29.530% |
| on_b | 895 | 30.397366446 | 29.4433 | 30.173% |

合并按总 Present / 总时间计算，OFF 1730 / 60.861043696，
ON 1790 / 60.912156677；门槛为 ≥2%，本轮为 +3.381%。
FPS 指成功 native Present，不是显示合成器上屏时间，也不是独立 Guest 帧计数。
每段 FPS 中全部 profiler 和 frontlock diagnostics 关闭，随后才开启单独的 Litep 窗口。

CPU ticks 合并 **29.545% → 30.244%**。
这是进程占所有 CPU 时间的比率，不代表每个大核都空闲。ON 翻译线程 CPU 约 24.62 ms/源帧，
配合约 29.4 FPS，粗略相当于单个核心约 72% 时间，仅用于说明总占用和单线程限制不是同一指标。
KGSL `gpubusy` 由应用状态采集器也在读取，本轮只保存最近区间原始值，不能当作与 30 秒 FPS 对齐的 GPU 利用率。
热服务前后均为 `Thermal Status: 0`；未调整热策略。

首轮 PID `3423` 完成前三段后，GameActivity 关闭并返回启动页，系统记录 `EXIT_SELF/status=0`。
没有本进程 fatal signal/device lost/断言证据。关闭原因没有进一步归因；完整退出日志保留在 `failure-*`。
该轮 `ab/` 只作失败采样证据，不混入最终合并结果。修复启动脚本后重新加载，用 PID `10583` 完整重测。

## Litep 等待转移（ms/完整源帧）

OFF 168 帧、ON 176 帧。各线程分开计量，父子范围和不同线程不能相加当作帧时间；
表中 0 表示所选窗口没有该等待范围，不表示所有运行场景永远没有等待。

| 指标 | OFF | ON |
| --- | ---: | ---: |
| M/P FrontLock | 1.7220 | 0.0000 |
| worker 0x58009 FrontLock | 2.0950 | 0.0000 |
| worker 0x6000a FrontLock | 1.2901 | 0.0000 |
| R FrontLock | 0.3906 | 0.0000 |
| M WorkerMgr::sync（完整调用） | 24.9032 | 18.5980 |
| M postCalc_（完整调用） | 22.7612 | 16.8971 |
| M CoreCheckpoint | 1.3926 | 1.3655 |
| R Translate elapsed | 25.0224 | 24.9244 |
| R Condition.Wait | 8.6369 | 7.3857 |
| M Publication.Drain | 0.1179 | 0.0081 |
| R Publication.Drain | 0.3293 | 0.5226 |
| worker 0x58009 Drain | 0.0823 | 0.0000 |
| worker 0x6000a Drain | 0.1549 | 0.0000 |
| R Translate ThreadCpuNs | 24.6821 | 24.6178 |
| R EngineLock.Wait（self） | 0.3223 | 0.5183 |

R 的 drain 从约 0.33 ms 增至约 0.52 ms，engine lock 也略增；已列入转移表，未把它们隐藏。
M/worker 不再负责大量 drain。R Translate 经过时间约 25 ms，CPU 约 24.6 ms，
翻译成本没有随本轮去锁显著下降，说明之后只盯 FrontLock 已经不够。
后台线程的 CoreCheckpoint 还有变化（例如 TID 11089 等待增加），完整表在 `analysis2/summary.json`；
没有 selected-notify 的新证据，不把这些范围直接认作关键路径或与主线程相加。

ON 的 R self 时间：Draw 17.068 ms/帧、
CpuRead.Preflight 3.864 ms/帧。
下一步是 `profiler_detail start 32` 配合关闭窗口量化扰动，再在这两个热点中定位 descriptor 维护、
UBO/vertex/index 快照、图像准备和编码的前两项。稳态新 PSO/Shader 为 0，不支持“正在大量动态编译”作为本轮主因。

## 队列与对象计数

| 窗口 | 入队增量 | drain 增量 | 累计 overflow | 窗口尾 pending | in_flight |
| --- | ---: | ---: | ---: | ---: | ---: |
| off_a | 88515 | 88515 | 0 | 0 | 0 |
| on_a | 91479 | 91479 | 0 | 0 | 0 |
| off_b | 88290 | 88276 | 0 | 26 | 0 |
| on_b | 91290 | 91308 | 0 | 0 | 0 |

max_depth=416（包含启动加载），capacity=4096，publication_failed=0；加载和稳态均未见 overflow。
状态快照时仍可能有正在生产的 pending 项，不能要求任意两个不同时间的计数逐项相等。

以下为 Present 边界的增量均值；PublicationDeltaValid 在所选窗口均为 1。

| 计数 | OFF / 帧 | ON / 帧 |
| --- | ---: | ---: |
| Native.ImagesEpochDelta | 82.7440 | 82.4375 |
| Native.TextureInitializationsDelta | 77.6786 | 77.5227 |
| Native.TextureFinalizationsDelta | 82.6548 | 82.4943 |
| Native.ImageIndexRebuildsDelta | 0.0000 | 0.0114 |

ON_A/B 未采样窗口内 Invalidate 均只走无 CPU 输出交集路径，conflict/unknown 增量为 0；
不能据此推断游戏其他场景没有写回，写回路径由专门 GPU 回归验证。

## 验证与限制

- Mac Metal、Mac Vulkan 构建通过；主机 porting 24/24、profiler 2/2；Android assembleBotwDebug、lintBotwDebug 通过。
- 两端都显式重链 probe。Apple M4 Pro / 精确 MoltenVK `8ea25172b62b40919bcb60d24c7d93970ac625921a38eb625e66d57f52e8ea8e` 与 Odin3 Turnip 各 5/5：OFF、ON、ordered、defer-events=1、defer-events=2。Mac 开启 Metal API validation。
- 实际 GPU 内容在 QueueFinish 前通过 Invalidate 校验：timestamp reports、带行距纹理拷贝、4 种 storage 写回链和事件；兼顾不相交/已退役范围进展、Image-only、混合输出、Unknown、同地址复用、物理/虚拟别名、原 range 开关关闭。
- 保留已有百万次对象/临时 builder 循环、4096 并发 recorder、66000 次纹理记录回收及所有像素、Fence、生命周期回归。
- 测试开发中修正了两处测试设施问题：复制 opaque pool cookie 会仍指向原对象，改成独立 boxed 值；当前移植未暴露 EventBuilder Guest API，事件回归改走已有后端命令解码。两次失败日志保留，不隐藏，也不宣称补齐了 Event Guest API。
- 没有做完整游戏画面差异验证、GPU/CPU 时钟校准、全场景跑图、调度器/KGSL trace。合成 probe 和稳定 Draw/Present 不能代替这些验证。W1 的完整 I2/I5 验收仍未补齐。

## PROF 完整性

| 窗口 | 采用源帧 | chunks | skipped / pairing / truncated | GPU begin/end/time/orphan | ROI open/unmatched |
| --- | ---: | ---: | --- | --- | --- |
| off_a | 85 | 374 | 0 / 0 / false | 344/343/682/8 | 73/0 |
| on_a | 88 | 371 | 0 / 0 / false | 359/359/720/12 | 104/36 |
| off_b | 83 | 359 | 0 / 0 / false | 335/336/670/6 | 103/30 |
| on_b | 88 | 372 | 0 / 0 / false | 354/354/708/6 | 115/41 |

四份均有完整容器和 payload，完整文件 scope_pairing_errors=0；各有 1 个 incomplete GPU zone 诊断。
ROI 的 open/unmatched 是裁剪边界范围，报告没有将其包装成无损、全闭合采样。
聚合器的 `capture_window_verified=false`、`observed_near_configured_limit` 保留；GPU/CPU alignment=unknown。
实际统计采用公开 index/query/load API，保留每份原始 PROF、源帧、显式 ROI 和完整诊断。

`off_b` 的 SDK176 是上一轮录制遗留标记，其结束跨过未录制段，长度 38.77 秒；
原始 live FPS 在该段正常前进。它不能算作一帧卡顿。保留排除理由和原结果，
重算只用 SDK177–259 的 83 个连续完整帧（3.022 秒）；其他窗口分别为 85、88、88 帧。
CPU 计数器按完成批次落入 ROI 计数，OFF 167 笔、ON 176 笔，全部 ThreadCpuValid=1；
它与经过时间 scope 的边界裁剪并不完全同口径，因此同时列出两者。

- `off_a`：`354ee76d75a6f8348e31829203b3954942fde3a6e111db04c41033ccf603febd`
- `on_a`：`f5ed020fe7c45ea815ff91ac616b25635cb5758b6254b8201369090798353d3a`
- `off_b`：`75174fb119593f816ab480aa9114a99c3bd57e5723dcb3a91da63bfde5f428e1`
- `on_b`：`5b08653a0267ab6cdc25b52a638f2c3c36ef0a5ec4d0cf564dd2834ab11a9ef8`

## 数据保全与交接

安装前后 78/78 个设置、缓存、存档文件哈希完全一致。游戏运行后仍为 78 个，无新增/删除；
只有游戏正常改写 `option.sav`，没有回滚。旧 APK、验证过成员哈希的备份 tar、安装报告和完整文件清单保留 Git 外。

最终 PID10583 健康，`publication_queue=1`、`invalidate_fast=1`、`invalidate_effective=1`；
所有 profiler/frontlock diagnostics 关闭，启动属性恢复到本轮开始值。
源码默认仍为 0，重启后不会自动保留这两个实验开关；没有把一次 A/B 当成修改发布默认的授权。

证据目录：`/Users/bytedance/workspace/native-switch-study/android-translate-20261026/publication-w2-20261028`。
本阶段差分：`android/build/publication-w2-20261028/change.diff`，由本阶段开始的逐文件快照生成，
不包含仓库原有的大量修改。不提交游戏数据、PROF、APK、符号或密钥。
