# 发布解耦 W1 实施与验收（2026-10-27）

**W1 未完成全部严格验收，按两轮迭代上限停止；性能收益已复现。** 最终 APK 同进程 OFF/ON/OFF/ON 合并为 **21.129 → 27.993 FPS（+32.49%）**，两对均提升。D1 对应“等待转移到其他持锁 API”，剩余入口为 `nvnMemoryPoolInvalidateMappedRange`。本轮没有实施 W2/W3、没有修改 Guest 同步语义，也没有改变默认值。

## 1. 为什么停止

任务书明确“同一阶段最多两轮实现 → ABAB；两轮都没达标就写报告停止”。第一轮发现 TextureCompare 去锁没有受开关保护，不能满足 I7；第二轮已补上并重新构建、重链探针、安装、ABAB。

第二轮 P/M 的 FrontLock 等待为 **1.529 ms/源帧**：相对本轮 OFF 7.847 ms 是 **19.49%**，但相对任务书指定的 W0 实测 6.264 ms 是 **24.41%**，仍高于 20%。同进程 A/B 更适合判断收益，因此两种口径都保留；不静默把 W0 验收基线换成本轮较慢的 OFF。跨进程差异本身不能解释为优化或回归。

I2 也尚未完全闭合：队列的 `retired` 统计退休请求，包含重新 Initialize 及按值副本重复请求；引擎沿用原有 identity 删除与 GPU 对象延迟回收规则。当前对账能证明 FIFO 操作没有丢失，**不能证明任务书要求的“每个旧 identity 恰好一次，且与 Finalize 对账”**。未新增永久去重表，也未为了满足计数而改变按值副本语义。因此不宣称 I1–I8 全部验收通过。

`publication_queue` 默认和最终运行状态均为 **0**。未提交、未推送。后续若按 D1 推进，只处理 Invalidate 的无 CPU 输出依赖路径，同时补齐 I2 的有界 identity 生命周期证据；不能直接开启默认值。

## 2. 实现范围

- Mac/Android 共用 Vulkan 前端；Metal 保持参考实现，未复制出 Android 专用分支。
- 新增独立互斥、有界 4096 项值队列：RetireTexture、RegisterTexture、RegisterSampler。Initialize/Finalize/Register 可先发布，再由最外层 FrontLock 排空到引擎；满时回到原同步路径。
- 持队列互斥时只复制值、更新 Guest 内联槽和原子计数，不获取 front/engine 锁。取出一批后释放队列互斥，再执行引擎操作。
- Submit 前额外排空；pool/descriptor pool/device 的既有 FrontLock 边界覆盖排空；关闭和 shutdown 先停止入队再排空。异常保留未完成批次并明确 fatal。
- TextureCompare 的去锁受相同开关控制；零初始化槽仍返回 false。生命周期计数改 relaxed 原子。TextureGet* / GetViewOffset / GetStorageSize 均未发现 FrontLock 或引擎调用，本轮没有调整 getter。
- `renderer_publication status|on|off` 支持热切换；ordered 模式拒绝启用新队列。旧 fence、命令快照、同地址复用检测和 GPU 延迟回收保留。
- GPU probe 缓存 GetProcAddress 的结果，避免每次查询函数指针都拿 FrontLock、意外提前排空队列而掩盖问题。

## 3. 身份与配置

- Odin3 `6edb0593`，包 `org.macnative.android.botw`，最终 `:game` PID **13794**。
- 最终 APK SHA-256：`15e75fcbdf7c80e0affa8377a24bdfcde21870c6d53ab3c00474c7655d739da9`。
- BOTW 1.6.0 main Build ID：`8e9978d50bdd20b4c8395a106c27ffde00000000000000000000000000000000`。
- Turnip SHA-256：`e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`。
- 720p 手持、内部 1×、realtime=1、shared Vulkan、ordered=0、live_range_flush=1、image_flush=1、PSO workers=auto（实际 4）。全部 stable 字段在四段前后相同。
- 自动读最新存档，持续满足 Draw≥1000、Dispatch、无新 PSO/shader、鲜活成功 Present 门槛。采样期间无输入注入；场景沿用复苏神庙测试存档，工具没有独立解析地图名和精确姿态。
- 四段 Thermal Status 均为 0。没有锁频、root、换驱动、清数据或卸载。

## 4. 未录制 ABAB

所有 FPS 段关闭 Litep、Guest tags/address/detail 和 frontlock 诊断。每段之后单独录制 3 秒 Litep。

| 窗口 | 成功 native Present | 秒 | FPS |
|---|---:|---:|---:|
| off_a | 648 | 30.425567 | 21.297878 |
| on_a | 856 | 30.475848 | 28.087816 |
| off_b | 637 | 30.392131 | 20.959373 |
| on_b | 849 | 30.432589 | 27.897725 |

合并使用总呈现数 / 总时长，不平均瞬时 FPS。第一轮 APK `c6bd6cfd…`、PID 5787 的结果为 21.381 → 28.417 FPS（+32.91%）；因 Compare 开关边界遗漏，只作初步证据，最终结论使用第二轮。

## 5. 等待转移表

线程角色按 Guest handle 固定：M/P=`0x8001`（宿主 13914，优先级 44、mask 1），R=`0x20004`（13979，30、mask 2），两个工作线程为 `0x58009`（13984，44、mask 2）和 `0x6000a`（13985，44、mask 4）。第二轮完整区间只有这四个线程出现 FrontLock.Wait，没有虚构第五名。

以下是各 ROI 内 elapsed 区间裁剪后累计 / 完整 source frame 数，OFF 共 121 帧、ON 共 161 帧。CPU 单独来自 Translate.ThreadCpuNs（每个样本 valid=1）。不同线程和嵌套范围不可相加成帧时间。

| 项目（ms/源帧） | OFF | ON |
|---|---:|---:|
| M/P FrontLock | 7.847 | 1.529 |
| 0x58009 FrontLock | 9.996 | 2.222 |
| 0x6000a FrontLock | 8.081 | 1.094 |
| R FrontLock | 0.236 | 0.132 |
| R Translate elapsed | 29.828 | 26.464 |
| R ReceiveMessageQueue 条件等待 | 18.640 | 8.853 |
| M postCalc_ | 35.181 | 23.289 |
| M WorkerMgr::sync 全调用点（含嵌套） | 36.803 | 24.729 |
| M drain | 0.000 | 0.034 |
| R drain | 0.000 | 0.163 |
| 0x58009 drain | 0.000 | 0.153 |
| 0x6000a drain | 0.000 | 0.123 |
| M WorkerMgr::sync 特定调用点 | 30.430 | 18.869 |
| M postCalc_ 内 SVC 0x34 | 17.534 | 12.874 |
| R Translate 线程 CPU | 29.309 | 25.963 |

特定 WorkerMgr 调用用 `Guest.Tag.CallerOffset=main+0x1507e7c` 对应任务书的 call 指令 `main+0x1507e78`，再按同线程 parent_sequence / zone_sequence 连接到函数区间；不是把所有 WorkerMgr 调用误算成这个调用点。SVC 0x34 取同线程在 postCalc_ 区间内的交集，包含阻塞/抢占，不等于 on-CPU。

每个 A 窗口最初 8 个完整源帧内各有 7 条完整 enqueue→selected-notify→resume 链，全部为 M/P `0x8001` 通知 R `0x20004`。按 epoch、wait ID、waiter 连接。R 的 SVC 0x1c 由同一 parent_sequence 的 Return0 记录对应 module 2（SDK）+`0x32637c`，与已有精确 SDK Build ID 的 vendor ReceiveMessageQueue 证据一致；SVC 0x1d 的 main 返回点另列，不混入消息等待归因。

ON 的 TextureInitialize/Submit 等待为 0；新的主要等待 API 是 Invalidate。`frontlock_status` 的诊断窗口略长于 PROF ROI，因此下表只作 API/持锁者归因，不拿它直接除以 ROI 帧数：

| ON 诊断窗口 | M Invalidate 等 Submit | 0x58009 | 0x6000a |
|---|---:|---:|---:|
| A（ms） | 186.998 | 317.041 | 89.251 |
| B（ms） | 191.221 | 218.458 | 165.815 |

FPS、消息等待、主线程 WorkerMgr 等待同步改善，支持“发布时被翻译锁阻塞”的 H1。收益没有被 drain 或翻译增加抵消；仍不能称串行环已彻底消除。D1 选择第三条：下轮只处理 Invalidate，参考 Mac `mtl_pool_invalidate` 的先查询输出范围再等待写回。该修改本轮未实施。

## 6. 队列与生命周期计数

| 未录制窗口 | 入队增量 | drain 增量 | overflow 增量 | drain 总毫秒 | drain 次数 |
|---|---:|---:|---:|---:|---:|
| off_a | 0 | 0 | 0 | 0.000 | 0 |
| on_a | 88261 | 88236 | 0 | 317.855 | 2213 |
| off_b | 0 | 0 | 0 | 0.000 | 0 |
| on_b | 90143 | 90143 | 0 | 275.956 | 2267 |

ON A 末尾还有 25 个 pending，后续关闭已全部排空；不能把瞬时 enqueued−drained 当成丢失。加载过渡也开启队列：最大深度 428/4096、overflow=0，关闭后 pending/in_flight=0。GPU probe 的极端批量测试可触发满队列回退，Mac 第一轮 ON 中 overflow=302，不属于稳态游戏溢出。

| Present 增量均值 | OFF A | ON A | OFF B | ON B |
|---|---:|---:|---:|---:|
| images_epoch | 82.900 | 82.901 | 82.852 | 81.963 |
| 地址索引重建 | 0.000 | 0.000 | 0.000 | 0.000 |
| TextureInitialize | 77.883 | 77.901 | 77.836 | 77.062 |
| TextureFinalize | 82.900 | 82.901 | 82.852 | 82.000 |

所有 PublicationDeltaValid=1。批量 release 与 Present 的边界不同，epoch 和 Finalize 增量可跨帧归属，不应强求逐帧相等。最终关闭队列：enqueued=drained=517914，retired=414180、registered_textures=103580、registered_samplers=154，三者和等于 drained；pending/in_flight/overflow=0。这是请求级对账，I2 的唯一 identity 对账仍缺失。

## 7. 测试和不变量覆盖

- Mac Metal/Vulkan、Android assemble + lint 均通过；每次改动后显式重链 Mac/Android GPU probe，OFF/ON/ordered 三种模式均通过。
- CTest 共 26 项通过。第一轮完整 Python 回归 342 项通过、1 跳过，porting Python 28 项通过；第二轮只改变 Compare 开关边界，重新执行构建/lint/CTest/六组 GPU probe。
- Queue 的 8 个生产者、8 万次并发发布，值复制、全序、溢出回退、关闭排空、异常保留/poison 测试通过。最新 Xcode 下 UBSan、ASan+UBSan、TSan 全通过；这些 sanitizer 覆盖队列主机测试，不代表全游戏均在 sanitizer 下运行。
- Compare 主机测试覆盖零槽、内存/描述/布局/swizzle 差异，结果与原实现相同。
- I1 的 retire→register→submit 顺序、I6 异常保留、I8 队列准入全序有主机测试；既有 GPU probe 保留同地址复用、录制后 finalize/再 submit、百万临时对象、4096 并发录制器、66000 纹理录制回收以及 pool 清理路径。
- I7 第二轮恢复 OFF 的 Compare FrontLock，ordered 的队列计数为 0；两端读回测试通过。I3/I4 的 GPU 内容与延迟持有沿用既有 probe 校验。
- I5 依赖所有既有 pool/device 生命周期入口的最外层 FrontLock 排空；尚无逐个入口、逐个 pending 类型的完整计数矩阵。与 I2 一样，不把一般 probe 通过扩大为未覆盖组合的证明。
- 自动读档和持续成功呈现通过；最终日志未发现 fatal signal、device lost、bad_weak_ptr 或断言失败。没有逐像素实机场景对照，因此不宣称所有渲染错误已解决。

## 8. Litep 完整性与窗口

使用公开 Litep `index_trace` / `load_prof` / `query_prof_sql`。SQL 查询按时间块和线程限定，保留每条查询及返回值；没有私有解码器。索引中的结束开放 frame 被排除。第一轮 OFF B 混入上次采集迟到的 marker，形成 40.777 秒跨采集间隔，已明确排除；不把它算成长帧或 FPS。

最终轮的连续完整 source frame 窗口：

| 窗口 | 起始 ns | 结束 ns | 完整帧数 | chunks | open spans | GPU orphan times |
|---|---:|---:|---:|---:|---:|---:|
| off_a | 445209792154324 | 445212770993699 | 60 | 334 | 79 | 4 |
| on_a | 445251425688178 | 445254392767605 | 81 | 366 | 107 | 24 |
| off_b | 445292792173074 | 445295764890730 | 61 | 314 | 87 | 2 |
| on_b | 445335979416928 | 445338960875001 | 80 | 358 | 112 | 20 |

四份均 PID 13794、skipped=0、pairing error=0、容器/载荷完整、未截断；各有 1 条 incomplete GPU zone 诊断。边界开放 scope 保留诊断，CPU区间按完整 marker ROI 裁剪。GPU/CPU 对齐仍 unknown，本报告不推断跨时钟 GPU 空闲或 CPU/GPU 重叠。

| 文件 | SHA-256 |
|---|---|
| off_a | `d9bffc7ee291d336136e544f6bc19832e7292c38b5cef308630e33a153961ba6` |
| on_a | `946a410582bb869634216f440c7b4fd9c839b0ac5a663d4f611aec2deb8a99c2` |
| off_b | `c7a5424e1a5598128fad4041fe48f461997adee40818822df324b55e4eb1f7d6` |
| on_b | `8641fc809835009a139bf864687d0e91c924acd9f2443f612f0920344ec881ef` |

## 9. 数据保全和最终状态

两次 W1 安装均在安装前后逐文件比对 78 个设置/缓存/存档哈希，全部一致；旧 APK 与 tar 备份保存在各自 deployment 目录。最终与 W0 前比对仍为 78 个文件，无新增/删除，仅游戏正常写入的 `option.sav` 改变，未回滚。

最终进程 13794 保持运行；publication_queue=0、frontlock=0，所有采样关闭，startup properties 恢复原值（guest_tags 从 1 恢复为空）。因此用户当前看到的是关闭实验开关的基线性能，不是本报告的 ON 帧率。未操作 Thor。

## 10. 证据和后续边界

- 完整证据：`/Users/bytedance/workspace/native-switch-study/android-translate-20261026/publication-w1-20261027`。
- 最终 ABAB：`iteration2/ab2/result.json`；具体配置差异会另写文件并中止，本轮没有差异。
- 完整逐区间 SQL：`iteration2/analysis/*-metrics.json`；通知链：`*-chains-filtered.json`；SDK SVC 配对：`off_a-r-sdk-paired.json`；归一化结果：`summary.json`。
- 最终状态/78 文件清单：`iteration2/final/`。各平台构建、lint、主机和 GPU probe 日志均保留。
- 本阶段专用 diff：`android/build/publication-w1-20261027/change.diff`；以 W0 完成时的文件快照为基线，不混入既有约 240 项修改。
- W2 只保存了修改前快照，没有代码变更或实机收益声明。W3 未开始；W4 的特定 WorkerMgr 调用点已补充等待量，但没有把任务名绑定到每个工作任务的起止；W5 沿用 W0 诊断，未放开 opcode 或有序翻译。

下一轮的具体入口已经可审查：Invalidate 先用独立输出范围注册表查询 `Writeback|Report|Event|Unknown`；仅无 CPU 输出依赖、非 alias、非 ordered 时绕过前端锁；有交集保持原路径。现有 Vulkan `cpu_access` 处理 writeback/report，Invalidate 额外处理 event；Image 自身不是该函数的 CPU 写回记录。实施前仍需针对真实 GPU 写回、事件和别名逐项证明，不能把 API 改成无条件 no-op。
