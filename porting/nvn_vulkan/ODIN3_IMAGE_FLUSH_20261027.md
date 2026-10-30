# Odin3：Guest 等待关联与仅图像 Flush 优化（2026-10-27）

本轮完成了共享 Vulkan 的小范围优化和实机对照，但没有解决主要低帧率：
四轮未录制测量汇总 **20.914 → 21.277 FPS（+1.73%）**，仍只有约 21 FPS。
重要结果是确认了等待会转移：Flush 工作线程不再等待翻译锁后，主线程更早到达
纹理初始化和 WorkerMgr 同步边界。不能把被消除的单线程等待直接当作帧时间收益。

## 实现范围

- `nvnMemoryPoolFlushMappedRange`：普通池、同步翻译、活动范围检查开启，且交集
  **只有 Image 租约**时执行 CPU 发布屏障，绕过 `front_mutex`。
- VkImage 使用独立设备存储，Image 租约说明图像内容所有权；单纯存在该租约不等于
  此次 CPU Flush 必须等候 GPU 向 Guest 内存输出。旧冲突分支只有全局锁与 CPU 屏障，
  本来就没有 GPU 等待／纹理写回操作。
- 写回、Report、Event、Unknown、物理／虚拟别名及有序翻译保留原保守处理。
  Invalidate、GPU fence、图像读回、命令快照及资源退休规则未改。
- 与 Mac Metal 对照的是“按实际 CPU 输出依赖同步”的原则：Metal 的 `g_wb`／
  `mtl_pool_invalidate` 按交集处理输出。Vulkan 本轮没有移植整个 Metal 内容合并机制，
  也没有把所有 Flush 变为无操作。
- `runtime_policy.h` 共用 `image_flush` 默认 1；Mac/Android 不各自维护实现。
  `renderer_flush_image status|on|off` 支持原子热切换；`NVN.Flush.ImagePublish` 与
  `flush_image_fast` 暴露实际命中。前置 `live_range_flush=1` 保持不变。

新增 `profiler_address start|stop|status`，默认关闭。在共享线程底座中记录真实地址等待的
Enqueue、SelectedNotify、Wake、Resume，包含 epoch、wait ID、对象地址、waiter handle、
notifier handle 和返回值。只有实际选中等待者时才记录通知；超时不补造通知。
对 CPU 事件使用物理 TID + 稳定 scope sequence，对跨线程链使用
`epoch + wait ID + object + waiter`，不靠事件排列位置或时间重叠推断唤醒关系。

## 实机身份与对照

- 设备 Odin3 `6edb0593`，进程 `org.macnative.android.botw:game`，PID **7159**。
- APK SHA-256：`d2d89383ed8ac2b80bdeb62e584ca897dc11872112073a6bc5bf5ebd9a1c0999`。
  与前版 `3005bd46…` 比较，ZIP 内容只有 `libmacnative_platform.so` 改变。
- BOTW 1.6.0，main Build ID `8e9978d50bdd20b4c8395a106c27ffde`，加载器收据确认
  `botw160.performance-tags-v1` 的 12 个已校验入口；没有引入新游戏地址补丁。
- Turnip SHA-256：`e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`。
- 手持 720p、内部 1×、realtime patch；共享 Vulkan 策略、PSO workers 4、
  `ordered_translation=0`、`live_range_flush=1`。没有改频率、驱动或存档。
- 自动读最新存档，等待持续游戏渲染、dispatch、缓存预热后测量；同进程不注入移动。
  这是原复苏神庙存档的固定视角比较，脚本本身不能识别具体地图或玩家姿态。

FPS 由成功 native present 的增量与样本年龄计算，不是 overlay 瞬时值或 GPU query 倒数。
每段约 30 秒，所有记录开关关闭；然后另采 3 秒 Guest tags + 地址等待 Litep。

| 轮次 | 图像 Flush | 成功呈现数 | 测量秒数 | FPS |
|---|---|---:|---:|---:|
| off_a | 关闭 | 634 | 30.395 | 20.859 |
| on_a | 开启 | 642 | 30.452 | 21.082 |
| off_b | 关闭 | 638 | 30.425 | 20.970 |
| on_b | 开启 | 653 | 30.413 | 21.471 |

+1.73% 是本次四轮汇总，样本量有限，不作为跨场景稳定提速承诺。
第一轮脚本在一个 off 样本后触发配置一致性保护而中止，保留在 `ab/`，
不混入上述 `ab2/` 完整四轮统计。后续脚本补充具体字段差异记录，第二轮全部配置检查通过。
首次中止没有保留字段差异，不能反推是哪一个字段变化。

## Litep 发现的等待转移

本轮 TID 7281 是 Guest 主线程，7351 执行同步 NVN 翻译，7435 是出现 Flush 的工作线程。
A 组 3 秒文件中完整 scope 的结果：

| 范围 | 关闭 | 开启 |
|---|---:|---:|
| TID 7435 的 FrontLock.Wait 合计 | 1210.87 ms / 85 次 | 未出现匹配等待 scope |
| TID 7435 的 ImagePublish 合计 | 无 | 0.203 ms / 610 次 |
| TID 7281 的 FrontLock.Wait 合计 | 0.442 ms / 6 次 | 486.21 ms / 66 次 |
| Translate，完整批次平均 | 27.82 ms / 61 次 | 29.97 ms / 60 次 |
| 主线程 postCalc_，完整调用平均 | 39.33 ms / 60 次 | 38.65 ms / 60 次 |

这些都是经过时间，不是 CPU 忙碌时间。不同线程、嵌套范围不能相加。
B 组也出现相同转移：工作线程 1209.70 ms 锁等待消失，主线程新增 409.16 ms；
Translate 均值 28.36 → 28.15 ms。因此 A 组翻译时间变长不能独立称为稳定回退。

在各自中间 1 秒连续窗口做显式地址链查询：

- off A：主线程等待对象 `0x2b6b69e20`，19 条完整链，实际通知者为
  Guest handle `0x1d0027` / TID 7435；入队到选中通知均值 **17.934 ms**，
  通知到 Resume **0.079 ms**。该直接等待位于 `gsys::SystemTask::postCalc_`，
  **不是**此前单独标出的 WorkerMgr::sync 调用。
- on A：该主线程与 Flush 工作线程的完整通知链不再出现。
- 另一处主线程 `sead::WorkerMgr::sync`，调用点 main+`0x1507e78`、
  返回偏移 `0x1507e7c`：off A 19 个完整调用平均 **19.368 ms**，
  on A 20 个完整调用平均 **34.516 ms**。不能把这一整段都当作空等；
  需要继续检查其中主线程参与工作队列处理的行为。
- on A 新增主线程前端锁等待的直接 API 父范围：`nvnTextureInitialize`
  **461.87 ms / 34 次**，占主线程此类等待约 **95%**；Invalidate 18.71 ms，
  Compare 3.23 ms，Finalize 2.25 ms，RegisterTexture 0.15 ms。
  这已把下一步定位到纹理生命周期发布边界，不能继续只扩大 Flush 放行范围。

原始数据中存在记录代际边界：on A 的最早残留事件可能早于当前 3 秒采集。
分析使用当前首次到末次 present marker 的连续窗口，以及窗口内部完整 scope／显式等待链；
不把残留事件跨度或两个文件之间的未记录时间算作帧时间。
A 组当前 marker 窗口：off `[438899522472871, 438902488414173]`，
on `[438940224722611, 438943216286517]`（CPU ns）。
地址链与调用点的子窗口是各自首 marker +1 秒到 +2 秒。

## 完整性与验证

- Android APK 构建、lint；Mac Metal/Vulkan runtime 与 GPU probe 构建通过。
- Mac schedule/catalog 回归 2/2 通过。
- 共享线程实际条件等待／地址等待路径验证通知者、唯一 wait ID、正常恢复、超时和关闭标签；
  普通编译与 UBSan 通过。本机 ASan 构建运行超时，未出现测试首条日志；独立一行 puts 的 ASan smoke
  同样超时且无输出，因此没有把 ASan 标为通过。失败与 smoke 记录保留。
- Apple M4 Pro / MoltenVK：关闭、开启、有序翻译三种 GPU probe 通过，Metal validation 开启。
  前两次启动参数错误没有进入 GPU，日志保留，不计为测试通过。
- Odin3 / 同代 Turnip：重新链接独立 probe 后关闭／开启均通过，各 407 次提交，
  包括像素回读、图像混合租约阻塞、百万对象循环、66000 次录制回收等。
  合成 probe 不验证 BOTW 全部游戏画面或 WSI。
- 四份 `ab2` Litep 文件分别解码 319、271、305、311 个 chunk；全部 0 skipped、
  0 scope pairing errors、容器及 payload 完整。每份保留一条 incomplete GPU zone
  边界诊断；A 组 off 首端 16 unmatched ends／尾端 63 open spans，
  on 首端 8／尾端 61。GPU/CPU 时钟对齐仍为 unknown，不作跨时钟瓶颈结论。
- 游戏进程保持运行，最终采样关闭、启动属性恢复。78 个原文件仍在，77 个不变，
  只有游戏正常写入的 `option.sav` 改变 4 字节；没有删除、增加或还原存档／缓存。
  最终日志未匹配 Fatal signal、device lost、断言或 bad_weak_ptr。
  本轮不以截图作为性能证据，也不宣称已完成长期跑图画面验收。

## 下一步

1. 对照 Metal，将 **CPU 纹理描述发布** 与 **旧引擎身份退休**分离，先针对
   `nvnTextureInitialize` 的 PUBLISH_LOCK；不能仅删除锁。
2. 对新 identity 的发布保留 Guest 外部同步规则；旧 identity 通过有界退休队列交给翻译
   所有者处理，命令快照、by-value 副本、同地址复用、pool 销毁和 GPU fence 仍须闭环。
   同时检查 Compare／Finalize／descriptor 注册是否只需要短 metadata 锁。
3. 使用本轮 Guest 调用点和地址链重复验证，检查 WorkerMgr::sync 与主线程直接 API 等待
   是否一起缩短；若等待迁移到另一个线程或阶段，不记作成功提速。

## 本地产物

源码目录：`/Users/bytedance/workspace/emulations/switch/MacNativeGames`。
构建／测试：`/Users/bytedance/workspace/emulations/switch/MacNativeGames/android/build/guest-flush-20261027`。
实机、备份、原始 PROF、索引、SQL 与分析：`/Users/bytedance/workspace/native-switch-study/android-translate-20261026/guest-flush-20261027`。

- `off_a`：`460cf49de1b3757a8de8b00e0403b76ed906bc2d0cc74b79bd6a8ee405b21750`（27135481 B）。
- `on_a`：`dde6e63c97c96be3220a7ed126da93aec5d83f0a282f6aadfc3f8eb85e15d8a3`（23358568 B）。
- `off_b`：`c769ecda8d1973e260a17d63ee533178f18fb951edfa2d1ca5c691bfd0215d07`（25950951 B）。
- `on_b`：`bbc533323592fe4586b6e215e84e8e0fb7f8fca034c528c75d00105305b81605`（26529610 B）。

Litep 索引 off A：`e2e71388288e44628133bcd215ca13bc`；
on A：`3efe803fab644f958b7207cea08e507d`。
分析 stage jobs 及查询返回保存在 `analysis/`，完整原始文件和 SDK sidecar 一并保留。
游戏资产、存档和原始 profiler 文件未放入 Git。
