# BOTW：统一呈现设置后的 NVN → Vulkan 并发与 pass 分析

## 结论

关闭 VSync、统一 120 FPS 上限后，差距仍然存在。复苏神庙同一存档、静止镜头的三轮短窗口：Metal 加权呈现速率约 82.14 次/秒；Vulkan 原来约 42.40，命令批量复制后约 44.64，短窗口改善约 5.3%。追加禁写附件修正后的稳定复测约 43.34，同一二进制关闭该修正约 42.48；不将这些顺序窗口的小幅差别当作跨场景收益。当前仍约 43 帧，接近两倍的差距没有解决，也未证明 CPU 或 GPU 已成为唯一瓶颈。

目前优先级是修正 NVN → Vulkan 的工作组织：附件读写模型、render pass 分组、提交时的数据快照和 Flush 可见性。不能将剩余成本笼统归给 MoltenVK。两条路径最终都使用 Metal，但交给 Metal 的命令结构并不相同。

## 对照条件与身份

- BOTW Switch 1.6.0，main Build ID `8E9978D50BDD20B4C8395A106C27FFDE`；同机 Apple M4 Pro，单个游戏进程运行，独立 profile 中相同存档。
- 两端手持模式、internal scale 1×，关闭 VSync，游戏上限 120 FPS、影片保持 30 FPS。关闭 Metal 的 keepalive、MFX 插帧/升采样、TAA。Vulkan 实际选择 `IMMEDIATE`，日志确认支持；不是仅设置一个未被使用的环境变量。
- Vulkan 使用 MoltenVK SHA-256 `8ea25172b62b40919bcb60d24c7d93970ac625921a38eb625e66d57f52e8ea8e`，UUID `12331E3A-07E0-3BE1-9BF9-EADB39C2733E`。显式设置 `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0`，driver 编码可以异步，NVN 翻译仍同步。
- D2R 参考 `758f5d41509af5cd38b7eb934bc2334bdb3e3342` 只读；所有引擎改动通过 `porting/nvn_vulkan/prepare_engine.py` 应用。Foundation `2f27d3bb41c1659d33b89905255fab2675fc0144`，Litep SDK `e00321ea40c4c608aff74a787200bc59493eea19`。
- 可执行文件按 SHA 冻结后运行。Vulkan 对照 PID 75982，SHA `05c5bc25b2085801f1b8341880ad6a57e591f1eeb607efd28a66d7296bcfb67e`；Metal PID 80840，SHA `de9514e5f612b19765969bd335424ac5e677e83342af917774d7dd6ccf1e0e78`；批量复制 Vulkan PID 87999，SHA `3c8ecc3f6d8cabb181caf3e3580fc9916c7059115833ef2e983883cc1faa0491`。

这些已结束进程的身份只属于对应历史采集，不代表当前前台进程。完整启动参数、Mach-O UUID、存档 hash 和日志保留于私有目录：

`/Users/bytedance/workspace/native-switch-study/macos-base-20261025/concurrency-20261025`

呈现统计采用 Vulkan 成功 present 完成计数、Metal drawable 回调计数。Metal 新增独立 `present_count`，避免把可能跳号的提交 ticket 当作完成次数。本次窗口两者增量一致。它们不是物理扫描 FPS，也没有公共、独立的 Guest 帧身份。draw 数约 3,650 与 3,920 并不完全相同，不能声称已经做到逐 draw 相同工作量。

| 实现 | 三轮呈现速率（次/秒） | 加权速率 |
| --- | --- | --- |
| Vulkan，逐条命令复制 | 43.34 / 42.38 / 41.49 | 42.40 |
| Metal | 82.72 / 81.44 / 82.27 | 82.14 |
| Vulkan，整段命令复制 | 43.99 / 44.43 / 45.50 | 44.64 |
| Vulkan，追加禁写附件修正，首轮 | 39.42 / 42.85 / 44.03 | 42.10 |
| Vulkan，追加禁写附件修正，稳定复测 | 43.35 / 43.31 / 43.37 | 43.34 |
| 同一 Vulkan 二进制，禁写附件修正关闭 | 42.36 / 42.77 / 42.31 | 42.48 |

每轮约 6 秒；测速期间不运行构建、Instruments、Litep 索引。原始状态序列与按 sample age 修正的时间保存在 `*-cadence-*.json`，汇总为 `cadence-comparison.json`。短窗口改善不能外推到其他场景或 Android。

后续全部窗口另保存在 `cadence-comparison-final.json`。禁写附件 A/B 使用同一 SHA `e2340a0ca60215a36a7622665ba278a9b60f3235a6ad64ec56ae0d750680bfcd`、UUID `A9F4AEEC-9346-3AA0-9C29-64E351820738`；开启 PID 5965，关闭 PID 13268。首轮波动原样保留，不能挑选最高窗口代表最后版本。

收尾恢复默认修正开启、诊断关闭的 Vulkan 进程 PID 15697：SHA `ac361ba50d09ede3229d4d9c392ba78be74311884b94f2f8a1b61b8672243633`，UUID `C82B98A5-3975-3848-A58E-A0F20FC1BC4D`。最后一处变更只把折叠 clear 的详细诊断标为 unknown，避免误记为未写；不改变渲染依赖。最终 Metal validation 探针再次通过 370 次提交，详见 `final-probe.log`。最终进程保留运行，采集关闭；启动时两条整数格式 blending 预热警告在旧基线也存在，未将其抹除或称为已经修复。

最终运行另一个 6.12 秒窗口为 42.18 次/秒，保存为 `vk-final-cadence.json`；状态、模块、实际 driver 映射和采集关闭信息在 `final-runtime.json`。因此交付状态应描述为仍约 42–44 帧，不能用中间最好的 44.64 宣称最终稳定提升 5.3%。

## 已实现的并发修正

1. CPU `nvnSyncWait` 使用独立短锁读取 sync 快照，等待目标 fence 时不持前端全局锁；引擎沿用固定 D2R 的目标 fence waiter 保护与有序退休，实际 GPU 等待期间释放 engine 锁。超时为零不会落入提交所有批次并等待的旧路径。
2. `nvnQueueWaitSync` 仍保留顺序锁。它是 GPU 队列依赖，不能用 CPU Wait 的解锁方式替代。普通提交槽压力、同步翻译、CPU 可见写回和 Flush 也没有全部异步化。
3. `CopyCommands` 对已冻结、已展开命令先校验整个区间，再一次分配/复制；避免每条 opcode 都查对象表和分配。纹理引用、Guest command-memory 计费与不可变副本保留。仍有中间字节副本，不是零复制。
4. 新增 `NVN.Translate`、submission token、`NVN.SyncWait.GpuUnlocked`、NVN barrier bits/open-pass 与 image-read hazard 采样；详细附件诊断由 `MNG_IMAGE_READ_DIAGNOSTICS=1` 显式启用，最多记录 128 个不同组合，默认关闭，不改变同步行为。
5. draw 时不挂载写掩码为零的颜色附件。保留原有附件尺寸计划；清除、折叠清除始终保留所需附件；pass fast cache 的身份加入有效颜色槽集合，保证掩码重新开启立即生效。它通过合法地省去未使用附件来避免 STORE 和假反馈，不通过删除真实读写 barrier 达成。`MNG_PRUNE_MASKED_ATTACHMENTS=0` 可做负向对照。

负向回归证明旧 SyncWait 会阻塞另一个线程的命令提交；新版本允许独立提交推进，并验证零超时 poll、较旧 fence 不能提前完成后续未 Flush 工作以及 2,048 像素 GPU 读回。Mac Vulkan 探针在同步/异步 MoltenVK queue-submit 两种设置下通过 366 次提交；批量复制默认与 `MNG_BULK_COPY_COMMANDS=0` 回退均通过。帧节奏测试覆盖 120/60 游戏上限、30 FPS 影片与原 Guest 间隔。

后续禁写附件回归通过 370 次提交：16 次连续采样绘制只开启 1 个 render pass，8,192 个读回像素验证目标输出、源内容保留、掩码重新开启及禁写槽的显式清除。关闭优化的负向对照在 `Sample-only color attachment split consecutive draws` 处如期失败。BOTW 实际 image-read barrier 从约 312 降至 310 次/帧，说明这一项确有误同步，但它只占当前问题的小部分；不能把整个问题宣布修好。

但实际场景中的 CPU SyncWait 已经走完成快路径：6 秒内 255 次仅约 **0.096 ms 总耗时**，没有 `GpuUnlocked` scope。因此它修复了并发契约，却不是这段 BOTW 的主要性能来源。

## 为什么优先修 NVN → Vulkan

### 附件反馈读取与 pass 组织不同

Vulkan 稳态每帧约 467 个 render pass，Metal 约 112–113。Vulkan 约 334 次 pass end 发生于 indexed draw（opcode 26），约 127 次发生于 NVN barrier（opcode 28）。不能把 opcode 26 解释成 SetRenderTargets；SetRenderTargets 为 8。

PID 87999 的 Litep 有 79,872 次 `NVN.ImageRead.Hazard`，值全部为 6：当前图像被标为写过，且仍挂在 open pass；没有一次来自布局转换。代码 `prepare_read` 按整个 Image 判断，`create_pass` 还会给全部附件刷新写标记。普通非 draw 命令通过 `Commands` 默认结束 pass 并插入全局内存 barrier。NVN barrier bits 虽被记录，当前执行没有按类型区分依赖范围。

同一采集的显式 barrier 分布：bits 18 为 35,646 次，255 为 12,522，32 为 3,728，0 为 2,556，17 为 1,280，1 为 1,168，27 为 1,024。零值还承担 tiled-cache action 的边界，不能直接全部删掉。

进一步核对 Metal 日志，下面的 fragment shader 在 Metal 使用 `fbread` 从 tile 读取自身附件，Vulkan 附件诊断出现相同 hash 前缀：

| Fragment shader 前缀 | Metal 日志中的附件 | Vulkan 诊断 |
| --- | --- | --- |
| `119f184b373462f4` | `13:3;15:7;17:1` | G-buffer 附件 3 被采样，读写同 mip/layer |
| `2ddf627033c66410` | `14:7;16:3;18:1` | G-buffer 附件 7 被采样，读写同 mip/layer |
| `1c3e5968474e37e6` | `18:5` | 附件 5 被采样，当前写掩码未开启该槽 |

Metal 的 `att_unstored_read`、`att_fb_take` 和 `g_sd_plain` 区分普通写入、附件采样连续段及可证明的同像素 tile 读取；不会因为每个采样 draw 都重新打写标记而必然拆 pass。Vulkan 当前缺少对应层。这是源码和运行日志之间的具体对应，但并不证明整个 1.84 倍差距都来自 framebuffer fetch。

随后在相同 Metal 二进制、相同设置下，仅设 `TOTK_ATT_FB=0`：PID 1502 的三轮结果为 86.36 / 87.38 / 83.96 次/秒，pass 仍约 113。单次顺序 A/B 的小幅差别不作为收益；它足以否定“缺少 tile fetch 本身就解释两倍差距”。更关键的是 Metal 在关闭 fetch 时仍保留反馈读取连续段，而 Vulkan 在每次 draw 上重复拆 pass。必须分别对齐读取语义与分组规则。

PID 98471 的附件细分采集包含 class 7（颜色、重叠、有写）75,838 次、class 5（颜色、重叠、该 NVN 槽未写）26,812 次、class 11（深度、重叠、有写）246 次。这里全部观测为同 mip/layer 重叠，不能据此把主要开销归给不相交子资源。该版诊断会把重复 NVN 槽分别计数，class 5 很多来自同一物理附件的禁写别名，**不能将 26,812 当作可删除的同步数**。后续诊断已按物理图像与 mip/layer 合并别名，并合并写集合；descriptor memo 也传递真实 shader view flags。

**不能直接删除 `prepare_read` 的 barrier。** Vulkan 的普通 `GENERAL` 布局不会自动提供附件反馈回路的全部保证；附件 load/store 也参与访问，颜色写掩码为零不等于没有 store 写入。应按 [Vulkan render pass 数据竞争规则](https://docs.vulkan.org/spec/latest/chapters/renderpass.html#renderpass-common) 建立合法的只读附件、input-attachment/subpass 或经过能力验证的反馈回路路径。当前固定 MoltenVK 源码没有 `VK_EXT_attachment_feedback_loop_layout` 实现，不能假定它可用。详细诊断只分类候选，不授权跳过依赖。

### 翻译和锁仍占有较长连续区间

批量复制后的 6.005 秒完整 marker 窗口有 254 个间隔；255 次 `NVN.Translate` inclusive 总计约 2.741 秒，约 10.75 ms/提交。最重 Guest lane 的 `NVN.FrontLock.Wait` 总计约 1.977 秒。两者有嵌套/并发，不能相加当成单帧耗时，也不能仅由重叠断言每次锁等待的持有者。

avp_study Time Profiler 的 5.5 秒窗口中，`inline_commands` inclusive sample weight 从前一窗口的 726 ms 降至 488 ms；两窗口节拍不同，不能解释为精确的单帧 33% 降低。新窗口 `xbg_submit_now` 约 2,034 ms、MoltenVK command encoder 约 1,027 ms，后者已出现在异步 worker。把 driver encoding 移走并没有消除 NVN 翻译本身或前端 Flush 的序列化。

本轮没有接入 Foundation JobSystem。后续先保留提交时 Guest buffer/descriptor/texture generation 的不可变快照，再将纯准备工作交给 JobSystem；有状态 Vulkan 翻译保留有序消费者。不能只把包含 Guest 指针的命令丢到 worker 后称为异步完成。

## Instruments 与 Litep 的证据边界

CPU/GPU 各录 8 秒，分析 1.5–7.0 秒。不同 trace 不拼接时钟。Vulkan CPU 9,373 样本中 654 缺栈；原始 DWARF 警告保留。

| GPU channel 区间并集，5.5 秒窗口 | Vulkan（复制优化前） | Metal（keepalive 关闭） |
| --- | --- | --- |
| Vertex | 1.384 s | 1.264 s |
| Fragment | 1.252 s | 1.212 s |
| Compute | 0.063 s | 0.214 s |

各 channel 可能重叠，不能相加当成总 GPU 利用率或将未覆盖区间直接叫 idle。应用区间开始到首个 GPU 区间的 p50/p95 为 Vulkan 4.32/11.45 ms、Metal 1.50/3.33 ms；包含 CPU 编码，不能称为纯队列等待。Vulkan 2,352 条、Metal 6,724 条记录缺少 encoding duration，尤其 Metal4 覆盖不足；禁止把已知 encoding duration 的总和当作完整后端成本直接比较。backdated signpost 警告保留。

Litep 文件 `litep-bulk/mng-botw-macos-87999-file-1792941833489492-1.prof` SHA-256 为 `2af620e8b51b06677c5aaffc6dfbcccf0d29663e9d9029495668166edeca63f6`。1,170 chunks、无跳块/截断、CPU pairing error 为 0；边界有 1 个不完整 GPU zone，GPU/CPU 对齐 unknown。详细插桩窗口约 42.30 次/秒，与无采集的 44.64 分开报告。附件细分另一次 PID 98471 采集 SHA `3b075e4c44b8c1d8c25d3cec4fb45cfa1de8e32f4d44430ffe437997723d653e`，不能混成同一帧。

## Android 验证

Odin3 `6edb0593` 已重新连接；普通 ADB/run-as，无 Root。相同共享逻辑通过 Adreno 830、无 libc++ 依赖 Turnip 的 369 次提交 GPU 探针，输出 `android-bulk-probe.log`。APK `botw-135-bulk.apk` SHA-256 为 `cb1d8b9f9451fe0fca7e4aeb5a40f700217a305ac62d54893943b37365916781`。该 APK 尚未安装，本轮没有 Android 游戏帧率结果。独立 probe 不能代替 BOTW 场景验收。

禁写附件修正后的 Android 探针又通过 373 次提交，包含上述连续采样/掩码/清除测试，证据为 `android-masked-probe.log` 与 `android-masked-identity.json`。对应 APK `botw-136-masked.apk` SHA-256 为 `804ddc7cc1127de3b91807ff19a524bc8e541e9ea06825dc7a977bfbeec91afe`，仍仅构建、未替换设备 App。

## 后续实施顺序

1. 以实际 view、写掩码、shader output、普通写/反馈读段和显式 NVN barrier 建立附件访问计划；先消除未使用附件的错误写标记，保持 clear/store 的真实副作用。
2. 对符合证明条件的同像素反馈读取实现 Vulkan input attachment / subpass 路径；其他反馈保留安全的版本快照或必要 pass 边界。按真实 driver 能力选择，Mac/Android 共享语义，不复制 Metal 的隐式行为。
3. 以 GPU 像素回读验证多次反馈绘制、深度只读、别名、mip/layer、显式 barrier、Finalize 与槽复用，再做相同场景呈现/CPU/GPU 对照。性能改善必须伴随语义回归通过。
4. 再拆提交准备与 Flush 的数据所有权，将可并行准备接入 Foundation JobSystem。保留有界票据、退出/错误传播及实际 GPU 完成后的资源退休。
