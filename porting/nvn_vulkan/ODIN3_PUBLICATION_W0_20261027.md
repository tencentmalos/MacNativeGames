# 发布解耦 W0 诊断与基线（2026-10-27）

W0 通过：同进程 ABAB 合并关闭 21.222 FPS、开启 21.213 FPS，差异 −0.041%，满足诊断开销 <1%；两次开启窗口的等待 API 前三名一致。进入 W1a/W1b，发布队列仍未实现、未启用。这里没有证明游戏性能提升。

## 改动与边界

`frontlock_status on|off|status|reset` 默认关闭，固定 128 项，按等待 API / Guest handle / 开始等待时持锁 API 聚合。持锁者可能中途变化，因此不是完整锁所有权跟踪。两段开启窗口分别有 8 / 3 个表满样本，总计仅 1.791 ms，单独记入 other，不丢弃总等待统计。

拆分有序输入拒绝 reason 5；仅记录子原因和首次现场，不增加 opcode 或快照预算。默认有序模式仍为 0。补充 Present 边界的 images epoch、地址索引重建和纹理生命周期增量。Debugbus/探针构建胶水是实现任务书命令和验证所需的最小配套改动。未改 Citron、Foundation 或 D2R 只读来源。

## 身份与配置

- Odin3 `6edb0593`，包 `org.macnative.android.botw`，`:game` PID `23470`。
- APK SHA-256 `c69ed9a228bb3aac9dcb7fa6fdf5a8439493e3a02c2d82b9c68dbc4ae8212b58`；与前版相比，仅 `libmacnative_platform.so` ZIP 条目变化。
- BOTW 1.6.0 main Build ID `8e9978d50bdd20b4c8395a106c27ffde00000000000000000000000000000000`。
- Turnip SHA-256 `e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`。
- 手持 720p、内部 1×、realtime=1、image_flush=1、ordered_translation=0；同一姿态不注入输入。完整配置、热状态和线程清单见证据 `ab1/result.json`。四段 Thermal Status 均为 0。
- 自动读最新存档并通过持续游戏负载门槛；未从截图推断地图或像素正确性。

## 未录制 ABAB

| 窗口 | 成功呈现 | 秒 | FPS |
|---|---:|---:|---:|
| OFF A | 653 | 30.4597 | 21.4382 |
| ON A | 642 | 30.5007 | 21.0487 |
| OFF B | 641 | 30.5141 | 21.0067 |
| ON B | 651 | 30.4512 | 21.3785 |

按总呈现数 / 总时长合并。前三名均为 TextureInitialize、MemoryPoolInvalidateMappedRange、TextureCompare。

## 谁在等待谁

`threads` 将每次启动变化的宿主 TID 映射为稳定 Guest handle：

| 角色 | Guest handle | 本轮 TID | 优先级 / 核掩码 |
|---|---|---:|---|
| M 主线程，也是 P 消息通知者 | `0x8001` | 23572 | 44 / 0x1 |
| R 同步 Vulkan 翻译线程 | `0x20004` | 23648 | 30 / 0x2 |
| 纹理初始化工作线程 | `0x58009` | 23653 | 44 / 0x2 |
| 纹理初始化工作线程 | `0x6000a` | 23654 | 44 / 0x4 |

两段 ON 合计，开始等待时持有者为 QueueSubmitCommands 的占全部锁等待 **98.482%**。按等待 API 排序：

| 等待 API | 累计等待秒 | 其中观察到 Submit 的比例 |
|---|---:|---:|
| TextureInitialize | 50.109 | 99.783% |
| MemoryPoolInvalidateMappedRange | 2.545 | 99.777% |
| TextureCompare | 0.334 | 0% |
| MemoryPoolInitialize | 0.150 | 100% |
| TexturePoolRegisterTexture | 0.145 | 0% |

不同线程的等待有重叠，不能把上表相加当成帧时间。Initialize 等待同时集中在两个工作线程和 M，不能只移除 M 的等待。

Litep 连续完整源帧 1–63，窗口 `442429154992980..442432109248605` ns，来源为宿主成功 Present 标记：

| 项目 | ms / 源帧 |
|---|---:|
| R Translate elapsed | 28.177 |
| R Translate 线程 CPU | 27.796 |
| R 条件变量等待 | 17.863 |
| M FrontLock 等待 | 6.264 |
| 0x58009 FrontLock 等待 | 11.094 |
| 0x6000a FrontLock 等待 | 8.649 |
| R FrontLock 等待 | 0.289 |
| M postCalc_ | 34.104 |
| M WorkerMgr::sync 全调用点累计 | 36.551 |

WorkerMgr 的数值包含嵌套等待和多次调用，不能与 postCalc 或锁等待相加，也不能直接当作特定 `main+0x1507e78` 调用点的值。

SDK SVC 0x1c 返回点 `sdk+0x32637c` 与已有精确 Build ID 的 vendor `nn::os::ReceiveMessageQueue` 证据一致。最初 21 个完整源帧内的 20 条完整 enqueue → selected-notify → resume 链，均由 M `0x8001` 通知 R `0x20004`。按 epoch、wait ID、waiter 连接，不用时间近邻补边。该子窗口链总等待 365.121 ms，与通知者 M 的 FrontLock 等待交集仅 3.172 ms（8 条链有交集）。这更像“翻译挡住生产，翻译结束后再等生产完成”的串行链，不能解释成 R 整段消息等待期间 M 都在等前端锁。W1 必须检验通知等待与整帧是否真正一起下降。

## 每帧计数

63 个 Present 增量样本均有效：images_epoch 平均 82.952（78–90），ImageIndexRebuilds 为 0；TextureInitializations 平均 77.937（73–85），TextureFinalizations 平均 82.952（78–90）。零重建仅指本次稳定场景和当前路径，不代表其他路径永不重建。

## 验证与完整性

- Mac Metal/Vulkan 构建、Android assemble + lint 通过；两端 GPU probe 均显式重链。
- 24 项 CTest、28 项 porting Python 测试通过。完整 Python 回归在正确环境下 342 项通过、1 项跳过。
- 初次完整 Python 回归使用默认 Python（缺 cryptography）和 Xcode 16.3（缺 Metal 4），因此失败；隔离依赖并指定 `/Applications/Xcode.app/Contents/Developer` 后通过，未改测试语义。旧工具链的 ASan condition probe 曾超时；新工具链完整测试已通过。另有 FrontLock 和生产 condition 路径 UBSan 通过。
- Mac MoltenVK / Android Turnip 各运行 OFF、ON、有序模式三组 probe 全通过，读回校验及百万次对象循环保留。普通模式分别 404 / 407 次提交；probe 不覆盖游戏 WSI 和全场景像素。
- PROF SHA-256 `f211f6770b88bd67ca1ccffb6e130dffe09c51c34e20128bdc28b6de35c87c73`，350 chunks，skipped=0，pairing error=0，未截断；capture-end open spans=77，1 条 incomplete GPU zone，orphan GPU times=0。GPU/CPU 时钟对齐 unknown。
- FPS 窗口采样全关闭；后续 3 秒 Litep 同时开启 Guest tags/address/condition，不能把这段插桩帧率当作未插桩基线。结束时均关闭。
- 安装前后 78 个设置、缓存、存档文件哈希一致，保留旧 APK 与备份；未清数据、未卸载、未 root。

## 证据和下一步

仓库外证据：`/Users/bytedance/workspace/native-switch-study/android-translate-20261026/publication-w0-20261027/`。其中 `ab1/result.json` 保留原始帧率、配置和锁表；`r-chains.json`、`r-chains-overlap.json`、`roi_scopes.json` 保留公开 Litep SQL 结果。索引 ID `1ef550ff9e9e4332ba476779e002d6c7`，聚合 job `5f5fff174b2a42ce8ca2a46a0a1ea474`。

本阶段差异：`android/build/publication-w0-20261027/change.diff`。下一步 W1a/W1b；所有新性能行为默认关闭，以相同配置进行下一轮 ABAB，不把 W0 的诊断通过当作优化收益。
