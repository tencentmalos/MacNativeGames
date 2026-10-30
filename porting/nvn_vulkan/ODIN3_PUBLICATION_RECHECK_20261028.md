# Odin3 发布队列复测（2026-10-28）

## 结论

按用户要求从 Thor 切回 Odin3。**现有 W1 APK 的性能收益复现：同进程 OFF/ON/OFF/ON
合并 21.338 → 27.977 FPS（+31.11%），两对均提升。** Odin3 的 OFF、ON、ordered
三组 GPU probe 均通过，包括 GPU report 与 Guest 时钟检查；没有复现 Thor 的失败。
这不代表 Thor 时钟问题已修复，也不是新的渲染器优化版本。

本轮仅复测既有产物，没有第三轮 W1 实现调参，没有安装 APK 或修改渲染器。
W1 的全部严格验收仍未通过：M/P 锁等待剩余 2.180 ms/源帧，唯一 identity 退休和
完整生命周期矩阵的证据缺口仍在。默认及最终运行开关均为 0。
D1 仍指向“等待转移到其他 API”，热点是 `nvnMemoryPoolInvalidateMappedRange`。
下一步入口保持共享 Vulkan 的 Invalidate 范围依赖判定；本轮没有实施 W2/W3。

## 运行身份与设置

| 项目 | 证据 |
|---|---|
| 设备 | Odin3 `6edb0593`，manufacturer `ayn`，device `sun` |
| 系统 | `qti/sun/sun:15/AQ3A.250728.001/eng.Odin3.20260616.191954:user/dev-keys` |
| 包 / 游戏 PID | `org.macnative.android.botw` / `24717` |
| APK SHA-256 | `15e75fcbdf7c80e0affa8377a24bdfcde21870c6d53ab3c00474c7655d739da9`，复用原安装 |
| main Build ID | `8e9978d50bdd20b4c8395a106c27ffde00000000000000000000000000000000`，由 live `guest_modules` 校验 |
| GPU / driver | Adreno 830 / Turnip Mesa 26.3.0-devel，git `68dafb89a2` |
| 驱动 SHA-256 | `e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`，已核对 PID 24717 的 maps 和映射文件哈希 |
| probe SHA-256 | `dd5de69498534b24be43d95ee2c3f7679ed2726cacab1ebc29d6def5d4068ea2`，沿用 W1 第二轮已重链产物 |
| 分辨率 | 1280×720 手持，internal scale 1×，realtime=1 |
| 共享策略 | ordered=0、image_flush=1、live_range_flush=1、preparation_plan=1、PSO workers=4 |

全程明确指定 Odin3 序列号。Thor 虽同时连接，本轮未操作。
四段配置前后均通过 stable 字段检查，Thermal Status 均为 0；这不证明设备从未发生频率变化。

## 启动与 GPU probe

验证开始时旧游戏进程 PID 5465 不在前台，读取到的 native 日志停留在资源校验。
先保存日志和身份，再停止该游戏会话进行 probe。无 APK 安装操作。

第一次 probe 调用遗漏新诊断目录下的私有缓存子目录，返回 `Missing private shader directory`，
尚未进入 GPU 自测。保留该启动错误记录，创建参数要求的目录后，复用完全相同的二进制运行：

| 模式 | 退出码 | 秒 | 结果 |
|---|---:|---:|---|
| publication=0 / ordered=0 | 0 | 1.687 | PASS |
| publication=1 / ordered=0 | 0 | 1.619 | PASS |
| publication=0 / ordered=1 | 0 | 3.660 | PASS |

probe 保留百万次对象循环、并发录制、同地址复用、GPU 读回、别名与写回等既有覆盖。
没有修改 Thor 上失败的时间戳断言，也没有换驱动。合成 probe 通过不代替实际游戏画面验证。

随后用 `android/tools/validate_scene.py` 自动读取最新存档。设备处于无密码锁屏，
解锁后关闭 Odin 设置浮层，并在 BOTW 前台重新启动诊断 Service；没有向场景注入跑图输入。
脚本完成“存档打开、Draw≥1000、有 Dispatch、无新建 shader/PSO、鲜活 Present”的持续检查。
加载后 10 秒 OFF 基线为 21.394 FPS。沿用复苏神庙测试存档；工具未独立解析地图名和精确姿态。

## 未录制 ABAB

每段 30 秒，所有采样和 frontlock 诊断关闭；段后另录制 3 秒 Litep。
整轮同 PID，无角色移动或设置修改。

| 窗口 | 成功 native Present | 秒 | FPS |
|---|---:|---:|---:|
| off_a | 654 | 30.518590 | 21.429562 |
| on_a | 864 | 30.526307 | 28.303457 |
| off_b | 647 | 30.451231 | 21.247088 |
| on_b | 838 | 30.308829 | 27.648709 |

合并使用总呈现数 / 总时长：OFF 为 1301 / 60.969821，ON 为 1702 / 60.835135。
它是成功 native Present 的实际节拍，不是 compositor 上屏时间，也不是插桩窗口的帧率。
相邻两对都提升，未通过额外轮次挑选结果。

## Litep 等待与翻译

Guest handle 保持：M/P=`0x8001`（TID 26487），R=`0x20004`（28141），
worker=`0x58009`（28147）、`0x6000a`（28148）。按 live `threads` 对应，未复用旧 PID 的 TID。

下表为连续完整 source frame ROI 内累计时间 / 帧数，OFF 共 122 帧、ON 共 164 帧。
scope 是 elapsed，含阻塞与抢占；不同线程和嵌套行不能相加。WorkerMgr 列是全部调用点，
本轮没有重做特定 callsite 与 SVC 0x34 的 token 连接，不能把它替代上一轮的窄调用点指标。
R 的条件等待按该线程 `Guest.Condition.Wait` 统计；本轮没有重新闭合每条 selected-notify 链。

| 指标（ms/源帧） | OFF | ON |
|---|---:|---:|
| M/P FrontLock | 8.187 | 2.180 |
| worker 0x58009 FrontLock | 9.710 | 2.163 |
| worker 0x6000a FrontLock | 9.151 | 1.367 |
| R FrontLock | 0.272 | 0.288 |
| R Translate elapsed | 28.831 | 25.747 |
| R Translate 线程 CPU | 28.381 | 25.341 |
| R 条件等待 | 18.776 | 8.996 |
| M postCalc_ | 35.369 | 23.648 |
| M WorkerMgr::sync 全调用点 | 37.623 | 25.522 |
| M drain | 0 | 0.121 |
| R drain | 0 | 0.312 |
| worker 0x58009 drain | 0 | 0.066 |
| worker 0x6000a drain | 0 | 0.004 |

Translate CPU 来自 `NVN.Translate.ThreadCpuNs`，122/164 个样本全部 valid=1。
R 的锁等待小幅上升约 0.016 ms/帧，没有抵消整帧收益；不能称所有等待均下降。
M/P 的剩余锁等待占本轮 OFF 的约 26.6%，占 W0 6.264 ms 基线的约 34.8%，
仍未满足任务书 ≤20% 的严格门槛。

ON 两段中没有 TextureInitialize/Submit 等待，新的主热点是 Invalidate：

| 诊断窗口，持锁者为 Submit | M/P Invalidate | worker 0x58009 | worker 0x6000a |
|---|---:|---:|---:|
| ON A 累计 ms | 196.419 | 287.147 | 124.874 |
| ON B 累计 ms | 215.103 | 274.596 | 121.523 |

frontlock 诊断窗口略宽于 PROF ROI，表中累计值不能直接除以 ROI 帧数。
持锁 API 是 try_lock 失败时的观测，不能当成整个等待区间的精确锁归属。
结果与上一轮的 D1 分支一致；下一步不应扩大 Flush 无条件放行范围或修改 Guest 同步语义。

## 队列与完整性

四段稳态 overflow 增量均为 0；本轮未做 publication 开启状态下的加载过渡测试。
最终 enqueued=drained=231286，retired=191927、registered_textures=39359、
registered_samplers=0，三者和等于 drained；pending/in_flight=0，max_depth=79/4096。
累计 drain 993.479 ms，单次最大 20.600 ms。该峰值覆盖整个对照过程，不是稳态均值。
这些仍是请求级计数，不构成 I2 的唯一 identity 退休证明。

| 窗口 | ROI 起始 ns | ROI 结束 ns | 完整帧数 | chunks | GPU orphan times |
|---|---:|---:|---:|---:|---:|
| off_a | 41070076938891 | 41073028183266 | 61 | 323 | 0 |
| on_a | 41111456670454 | 41114424826391 | 81 | 361 | 4 |
| off_b | 41152062307016 | 41155017394412 | 61 | 320 | 2 |
| on_b | 41194109300454 | 41197105372329 | 83 | 358 | 2 |

四份均 PID 24717、skipped=0、scope pairing errors=0，容器和载荷完整、未截断。
每份各有一条 `dropped incomplete gpu zone` 诊断；边界开放 scope 记录保留。
所选 ROI 均为 continuous single generation，无内部录制边界。
后三份文件索引的最早事件时间延伸到上一段采样尾部，故不能把整文件首尾跨度当成本段录制时长；
只使用上表连续完整 marker→marker 区间，排除末尾缺少结束 marker 的帧。

GPU/CPU 对齐仍为 unknown。NVN Guest report 自测通过不等于 PROF 的跨时钟校准通过，
本报告不据此推断 GPU 空闲、CPU/GPU 重叠或任一方已经跑满。
查询经公开 Litep `index_trace` / `load_prof` / `query_prof_sql`；两条初始宽 Counter SQL
超过 5 秒限制，保存失败记录后增加 R 的 TID 过滤，按 0.5 秒片段完成，没有私有解码器。

| 文件 | SHA-256 |
|---|---|
| off_a | `d76ae6774eb24721845999ce1cee7972cba83c9ae9d179a6114dbe6cfaefd75d` |
| on_a | `ccd158ac2cd7c74445e2fdf160fb90c5688a1cc7cee2c4db2b48dd961755ad4b` |
| off_b | `b06ebb643bcb6ddf1e845dd7bfd111c6ba131eb86e5e9bbe4038c634df38dd17` |
| on_b | `c7dc89f73ba5a2dd73ad8be438651420735bc1300c2df0f75da0211b2a37de89` |

## 保全和最终状态

78 个设置/缓存/存档文件前后均存在，无新增或删除；只有游戏正常写入的 `option.sav`
哈希改变，未回滚。没有安装、卸载、清数据、root、锁频或换驱动。
启动属性全部恢复原值，所有采样关闭，publication=0，队列已排空。
游戏 PID 24717 留在场景中，因此最终运行的是约 21 FPS 的 OFF 基线；约 28 FPS 是明确开启
实验队列时测得的结果。未修改默认值，以免把尚未闭合的 W1 验收宣称为完成。

最终 native 日志未发现 fatal signal、device lost、bad_weak_ptr 或 assertion failed。
本轮没有逐像素实机场景比较，不能宣称所有视觉缺陷已修复。

## 证据与后续入口

证据根目录：
`/Users/bytedance/workspace/native-switch-study/android-translate-20261026/odin3-publication-recheck-20261028/`。

- `probe-setup-results.json`、`probe-results.json`、`probe-*.log`：启动错误和三模式正式结果。
- `scene/result.json`、`scene/events.jsonl`：自动读档和稳定负载门槛。
- `ab/result.json`、四份 PROF 及 sidecar：同进程完整原始对照。
- `analysis/`：公开工具原始返回、ROI、SQL、完整性、逐窗及合并指标。
- `live-driver.json`、`live-maps.txt`、`final-modules.txt`：运行中身份。
- `file-preservation.json`、`final-state.json`：文件保全与恢复状态。
- 阶段差异：`android/build/odin3-publication-recheck-20261028/change.diff`，仅本轮文档。

接下来的实施仍应只处理共享 Vulkan 的 Invalidate 热点：无 Writeback/Report/Event/Unknown
交集且非 alias、非 ordered 时才考虑绕过前端锁；真实 GPU 输出仍保留等待和可见性。
同时补齐 W1 identity/lifecycle 证据，继续使用同进程 ABAB，不能直接默认开启队列。
参见 [任务书](SPEC_PUBLICATION_DECOUPLING_20261027.md)、[W1 验收](ODIN3_PUBLICATION_W1_20261027.md)
和 [Thor 停止记录](THOR_PUBLICATION_20261027.md)。
