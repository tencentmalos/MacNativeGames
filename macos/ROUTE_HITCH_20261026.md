# 复苏神庙短距离跑图与翻译检查优化

本轮在出生点读入同一存档，向前输入 1200 ms，保留静止基线、移动片段和相邻帧。
确认复现了约 38/46 ms 的连续慢帧，追加采集还出现 73 ms 慢帧。证据包含 Guest 侧等待
和首次 Shader 资产读取两类问题；不能只归因于 Vulkan Pipeline 编译，也不能宣称
已修复全部卡顿或追平 Metal。

## 实现和验证

- 共享输入层增加有界 `input_pulse`。Mac Metal/Vulkan 与 Android 的 debugbus 都已注册；
  默认不产生输入，最长 3000 ms，到期自动释放，真实输入会取消。新增请求、实际 HID
  消费和释放的 Litep 标记。`macos/route_capture.py` 固定 PID、采集 ID 和输入参数，
  保留采集清单并在异常时只清理自己创建的操作。
- NVN 读取预检在只剩计数器报告时跳过普通 uniform、vertex 和原生 index 缓冲的
  无效扫描。这与原 `cpu_access(reason=1/2)` 不消费报告的语义一致；实际 pending
  GPU 写回、存储写入、纹理、8 位 index CPU 转换和 UI CPU 读取仍保留检查。
  `renderer_schedule reads on|off` 可在 engine 边界切换；不改变快照内容和退休规则。
- Flush 增加默认关闭的 32 项范围采样。实测既有当前不对应图像的 12032 B 范围，也有
  对应 GPU-written 图像的 2 B 更新。当前状态查询不证明调用时依赖；保守 Flush 锁未删除。
- 调度层新增 `Sync.CoreSchedulerLock`、`Sync.VirtualCore.Wait` 和等待入口 owner 快照。
  不改变优先级、亲和核或抢占策略；信号处理器不调用 profiler。
- Mac Vulkan 与 Metal 均编译通过。读取过滤开、关各通过 384 次提交的 Vulkan probe，
  包含 GPU 写回链、局部重叠、别名、生命周期与呈现引用回归。Python profiler 测试 8/8；
  实机验证非法输入参数、stop、HID 消费和到期释放。Android 注册已更新，本轮未构建 APK，
  `profiler_test.cpp` 新增协议测试尚未在 Android 执行。

## 同进程静止 A/B

PID 34003，二进制以 `preflight-filter-identity.json` 的完整 SHA 和 UUID 固定。
720p 手持、1x、vsync off、帧率上限 120、keepalive/MFX/TAA off；批次 1024，
adaptive/present/range-invalidate 维持开启，MoltenVK 异步 queue submit 维持原设置。
两份 Litep 文件各 3 秒，细分 CPU 与细分 GPU 关闭。每项按完整 `NVN.Translate` 次数
归一化；不同线程的范围有重叠，不可相加当作帧时长。

| 范围 | 过滤关闭，178 次翻译 | 过滤开启，184 次翻译 |
|---|---:|---:|
| `NVN.CpuRead.Preflight` | 1.265 ms | 1.152 ms |
| `NVN.Translate` | 8.445 ms | 8.332 ms |
| 主要 Flush 线程的 `NVN.Flush.Conservative` | 4.809 ms | 4.901 ms |

预检局部耗时下降约 8.9%，翻译下降约 1.3%，保守 Flush 没有改善。
关闭录制的六轮上屏完成率，off 为 62.79/56.77/62.97，on 为 59.42/61.57/62.93。
两组均值 60.85/61.31，中位数 62.79/61.57，波动大于局部变化，**没有稳定 FPS 提升证据**。
这里是成功 native present 的完成率，不是独立验证的 Guest 帧率。

## 首段慢帧

PID 30655 的移动采集，373 个完整 source frame，中位数 15.348 ms；
原始 SDK frame 521/522 分别为 38.214/46.200 ms。向前脉冲发生在
CPU 时钟 `80171794776708` ns，两帧都位于请求的 1200 ms 移动区间。

frame 522 对比连续邻帧的中位数 16.920 ms：

- 呈现线程有 37.294 ms 无 scope 覆盖，不能把它命名为 GPU idle 或锁等待。
- `Host.Present.CopyFence` 8.747 ms，比邻帧中位数增加 1.665 ms。
- 翻译线程 TID 2020979 的 `Sync.GuestCondition` 覆盖 38.860 ms，其中一段完整等待
  为 40.623 ms，跨越帧边界。随后实际 `NVN.Translate` 仅 7.260 ms，邻近翻译为
  7.662/7.876 ms，没有几十毫秒的翻译突增。
- TID 2021028 的 `Sync.CoreCheckpoint` 完整范围为 29.994 ms；帧内裁剪后的累计范围
  30.406 ms。另一条 Guest 线程同期存在未覆盖区间。此处缺少 owner 与通知身份，
  只能视为调度调查线索，不能由重叠时间断言谁阻塞谁。
- 整段只有 4 次 `Host.Pipeline.Compile`，合计 1.581 ms；上述慢帧窗口内未观察到该调用。

因此下一步应沿“Guest 任务/虚拟核等待 → 提交间隔 → 呈现”补齐依赖证据，
而不是继续只削减 Vulkan driver 编译或扩大在途槽数量。

## 证据身份和限制

所有原始文件位于本机工作区外的：
`/Users/bytedance/workspace/native-switch-study/macos-base-20261025/concurrency-analysis-20261026/`。
ROM、存档和完整采集不进入仓库。两份存档哈希在各次启动清单中保持不变。

| 证据 | 文件 / SHA-256 / Litep 身份 |
|---|---|
| 首段移动 | `route-forward/mng-route-30655-file-1792992032879422-2.prof`；`c2938fdb4388a5ab2e3908a438232086b080199bb4bebd8059dd3bfba90a785a`；index `f70e5d62500f49828a1d7e56aa2d547d` |
| 过滤 off | `filter-off/mng-filter-34003-file-1792992545272084-1.prof`；`daba95b776653850e36f97ba7632f525307aee70a16018a89284c59efd52bb89`；job `5a7eb27596d048ac8e99f25fca71f937` |
| 过滤 on | `filter-on/mng-filter-34003-file-1792992561421488-2.prof`；`a810c8ba1631ee5e6774645cfe783686f36542347159fa4e5e14884fe58ee302`；job `23059311280649178bc66a3e68fbd412` |
| 调度补桩后移动 | `route-core/mng-core-37926-file-1792992977669998-1.prof`；`bf9be39ddea3cb08c7708b9b8457849792097a2aedee88cc6532633ec2e22abe`；index `d744c27f627845e3b67639dc4a9af60a` |

前三份均容器/payload 完整，无跳过 chunk、无 scope 配对错误、无内部 recording 边界。
移动采集有 GPU orphan time 6；off/on 的 CPU 切边开放 scope 为 25/22，GPU orphan time
为 0/2，均有不完整 GPU zone 的边界诊断。GPU/CPU 对齐仍 unknown；不能据此声称 GPU
利用率、饱和度或跨时钟关键路径。`route-frame-522.json`、`frame521.json` 保留逐帧分解。

调度补桩验证二进制 SHA-256：`244570932ce9a6858bda6185bd829676a6bdde1a6fb5d88c41ea81409a021d96`，
UUID `959F8827-1249-3C80-892F-A44249778582`，PID 37926；源码见 `route-source-hashes.json`。
已核对实际加载 MoltenVK 1.4.2 本地诊断库，沿用上一轮驱动，不切换 Citron 发布基线。
调度补桩后的路线确认 HID 消费 generation 1 且自动释放；清单 `route-core-capture.json`。
输入请求与 HID 消费不能单独证明两个后端人物位置完全一致，本轮没有新的 Metal 游戏实测。

## 补桩后发现两类慢帧

PID 37926 的 2 秒路线有 118 个完整帧，容器/payload 完整，0 跳过 chunk、0 配对错误、
29 个切边开放 CPU scope、0 GPU orphan time，仍有不完整 GPU zone 的边界诊断。

- frame 51 为 73.076 ms。翻译线程 TID 2062015 的 Guest 条件等待为 62.799 ms，
  后续翻译仅 8.145 ms。TID 2062026 的 `Sync.VirtualCore.Wait` 为 55.756 ms；
  入口快照显示等待线程 handle `0x8800f`、priority 48、mask `0x2`，核 1 当时的
  owner 是 `0x58009`，通过同 PID 的 `threads` 快照映射到 TID 2062020、priority 44。
  该入口快照不能证明 owner 在完整等待期间未改变，暂不修改优先级或核亲和策略。
- frame 22 为 34.077 ms，同步翻译 26.255 ms，其中翻译自身未细分部分 17.506 ms，
  比邻帧中位数多 16.598 ms；Pipeline 编译仅 0.888 ms。持锁加载新的预编译文件成为
  第二条调查线索，源码的首次加载路径和日志给出单个文件 1–2 ms 读取。

后续增加 `NVN.Shader.Load/Read/Key64`，并实现可选的只读连续归档。
目录总计约 6 万项，其中实际 XGS/XGV 为 19,791 项；归档 877,366,124 B，SHA-256
`a3da6f7d9b52b3efd7972691b394cf33af7a432fce79702d167b57b149b3dc59`。
全部归档负载逐项匹配源 SHA，9 项读取器/打包测试通过；散文件、归档各通过 384 次提交
Vulkan probe。实现及发布约束见 [Shader 归档](../porting/nvn_vulkan/SHADER_ARCHIVE.md)。

## 归档实机验证结果

散文件 PID 42009 与归档 PID 43447 使用完全相同的二进制 SHA-256
`ada943ef96690ecda39a009a2d635526746db3523ee0e1bf523c023cae297e91`，UUID
`53304BF4-5681-3E3E-99FA-6CCD1FE6FA09`。环境唯一差异是 `MNG_SHADER_ARCHIVE`；
存档 SHA、画质、驱动和调度设置一致。已通过 lsof 确认后者确实映射归档和原 MoltenVK。

两次从同一存档读入并稳定后，采 2 秒、在 0.2 秒发送向前 1200 ms。两段分别出现
23/19 个首次加载 Shader，共同 19 项的 key、stage、word 数完全匹配；这 19 项日志读取
耗时合计为 **8.85 → 0.04 ms**（日志精度 0.01 ms，不代表零成本）。
`archive-route-shader-comparison.json` 保留逐 key 对照。

Litep 基础范围也确认读取段明显缩短：

| 整段范围 | 散文件 | 归档 |
|---|---:|---:|
| `NVN.Shader.Read` | 46 次，10.893 ms | 38 次，0.085 ms |
| `NVN.Shader.Load` | 23 次，16.737 ms | 19 次，4.051 ms |
| `Host.Pipeline.Compile` | 4 次，1.809 ms | 4 次，1.392 ms |
| `NVN.Translate` | 121 次，1093.174 ms | 121 次，1027.696 ms |

两段首次加载的 Shader 集合不完全相同，Pipeline cache 也按原策略持续保存，
因此不能把整段翻译差异或 source frame 数直接作为严格的总体 FPS A/B。
本轮成立的结论是移除了逐文件读取的大部分成本；Guest 任务等待和 SPIR-V 元数据分析
仍可能造成剩余长帧。没有宣称 73 ms 卡顿已关闭。

- 散文件 PROF：`archive-loose/mng-loose-42009-file-1792993877178120-1.prof`，
  SHA `3701278b61bd09b42bc568dd852c9835935690ddf0557dec3c5e8217cf56ea6a`，
  job `1c279b4826f34ccfaa664d9107c89e56`。
- 归档 PROF：`archive-packed/mng-packed-43447-file-1792994004888788-1.prof`，
  SHA `9dbd0cd777b299241807fbbec19e660fe4e7f3e15b3042fe77e312fdcc78d089`，
  job `407a250d98844c0b865cfe04af0fac5c`。
- 两份均完整、无内部 recording 边界、0 跳过 chunk、0 配对错误；GPU 存在切边不完整
  zone，CPU/GPU 对齐 unknown。完整报告为 `archive-loose-stage.json`、
  `archive-packed-stage.json`。
- 最终源码清单 `archive-source-hashes.json` 覆盖 20 个文件，校验通过；Python/C++
  读取器与 profiler 工具测试合计 17/17。应用保持 PID 43447 运行，ring/detail/streaming
  已关闭，输入自动释放，存档 SHA 未变。移动后静止状态的成功上屏完成率约 64.30/s，
  不是与 Metal 的新同场景对比。
