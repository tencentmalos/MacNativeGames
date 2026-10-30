# BOTW 复苏神庙：Metal / Vulkan 性能对照

后续已统一关闭 VSync、设置 120 FPS 上限并关闭 Metal keepalive，结果及进一步的锁、命令复制、附件反馈分析见 [并发与 pass 分析](CONCURRENCY_20261025.md)。本文保留此前采集的设置、进程和结果，不与后续不同配置的数字混用。

本轮使用 avp_study 的 `reality-trace`，分别抓取原生 Metal 与
NVN → Vulkan → MoltenVK 的 Time Profiler 和 Metal System Trace。
不使用截图估计性能；场景图仅用于确认同一存档、位置和视角。

## 复现条件和证据

- Apple M4 Pro，Switch BOTW 1.6.0，main Build ID
  `8E9978D50BDD20B4C8395A106C27FFDE`。
- 同一份复苏神庙入口存档，Link 静止面对门；默认手持模式，internal scale 1×，
  输出 crop 1280×720。两个窗口同为 1510×1004 points。
  Vulkan 仍分配 1920×1080 输出纹理，crop 不代表全部附件尺寸。
- 原生 Metal 使用当前 v8 shader seed；启动预热结束后采集。
  其默认显示节拍为 120 Hz，并开启 GPU keepalive；Vulkan 主机 present interval 为 1。
  因帧节拍、shader 路线和后端功能不同，不把跨后端呈现率当作严格的游戏 FPS 倍率。
- Xcode 16.3 Instruments 录制，每次 8 秒，分析窗口 1.5–7.0 秒。
  CPU/GPU 分开串行录制，关闭 Litep ring、GPU capture 和 validation；无并发构建或解析。
  native reader 验证私有 ABI 和输入 hash，导出表非空且 complete。
- 证据根：`/Users/bytedance/workspace/native-switch-study/macos-base-20261025/perf-vulkan-metal-20261025`。
  原始 trace、导出 SQLite、日志、存档 hash 和运行身份都保留在该私有目录。
  原始存档、游戏文件和 shader 不进入 Git。

| 运行 | PID | Mach-O UUID | SHA-256 |
| --- | --- | --- | --- |
| Vulkan 原始采样 | 43514 | 023974DE-6BCE-324C-8703-2DA2A49E0C80 | 50f61df1831bea29d1fa0ad5504b3cae8c18a7fed5d9e2ccb3c460ac29cff055 |
| Vulkan 显式前台复测 | 53654 | 同上 | 同上 |
| 原生 Metal 场景采样 | 51080 | DB16935D-C9F8-3C8A-9561-D2949C94F6C2 | 0d1fd60a454c8ab860ecd971c535ea83573e512dbdb89a10d0ed8acaeb6df161 |
| Vulkan 延迟回收修复 | 55639 | 4CDE4905-9788-3394-839F-8EC7225232B0 | 382be22ecf78bf0ed140dc295de2834b607325e3329d6f4214853241d456e524 |

MoltenVK 始终使用同一份独立诊断库，SHA-256
`8ea25172b62b40919bcb60d24c7d93970ac625921a38eb625e66d57f52e8ea8e`，
UUID `12331E3A-07E0-3BE1-9BF9-EADB39C2733E`。
未修改 Citron 的手工测试驱动基线。

## 已定位的问题

原始 Vulkan 的 5.5 秒 CPU 窗口有 8,005 个样本，457 个缺少完整栈。
`nvnQueueSubmitCommands → xbg_submit_now` 的 inclusive sample weight 为 2.030 秒，
其中 `MVKCommandEncoder::encode` 为 0.817 秒；`inline_commands` 为 0.579 秒。
这些是跨线程 CPU 采样估计，嵌套 inclusive 不能相加，也不是函数经过时间。
稳态状态读数中 pipeline/shader 新建数为零，没有证据把当前低速归因于反复动态编译。

CPU 栈捕获到 `nvnTextureFinalize → xbg_legacy_finish → vkQueueWaitIdle`。
143 个含 `vkQueueWaitIdle` 的样本中，130 个还含 `nvnTextureFinalize`。
源码确认每次纹理销毁都会无条件清空整条 GPU 队列，属于资源生命周期与同步职责混用。
MacNative 的 `hle_nvn.c` 则退役纹理 identity；异步翻译时按队列顺序处理副本，
不会因为一个纹理句柄消失而等所有 GPU 工作。

独立的 6 秒 Litep 捕获含 197 个 present marker；首末 marker 间 196 个间隔，
跨度 5,987.08425 ms。多个 Guest 线程的 `NVN.FrontLock.Wait` 分别达到
2,024、1,511、1,162 和 837 ms，不能跨线程求和。前端全局锁仍包住同步翻译、
部分 CPU cache maintenance 和 Fence 等待；这条串行链尚未完全解除。
`Host.Present.CopyFence` 在独立线程等待 1,549 ms，不能简单删掉或算作 CPU 忙。
Litep GPU/CPU 时钟对齐为 unknown；一个边界 GPU zone 不完整，19 个结束边界 CPU scope 开放，
原始警告保留，不补零也不拼接不同采集的时间戳。

## GPU 时间线的解释边界

| 5.5 秒窗口 | Vulkan 原始 | 原生 Metal |
| --- | ---: | ---: |
| Vertex 区间并集 | 1.193 s | 0.705 s |
| Fragment 区间并集 | 1.064 s | 0.693 s |
| Compute 区间并集 | 0.052 s | 3.089 s |
| 应用记录 | 14,647 | 11,305 |
| 应用记录缺少 encoding duration | 13,922 | 7,747 |
| 未匹配 GPU 的应用记录 | 13,922 | 0 |

channel 之间会重叠，不能相加冒充总 GPU 利用率。缺字段的应用记录保留 unknown，
不能全部称为“空 GPU 提交”。原生 Metal 源码和启动日志证实 GPU keepalive 在独立队列
持续提交短自旋核；当前捕获没有可用于逐核归属的标签，所以不把 Compute 时间全部
归因于游戏 shader，也不声称已精确扣除 keepalive。
两组 GPU 录制都有 backdated signpost 警告，保留于录制日志。

## 本轮修正

`nvnTextureFinalize` 只删除 identity，不再调用 `xbg_legacy_finish`。
`image_storage` 保留 GPU 内容；提交槽和 present ticket 持有引用，pool 删除或 storage
sweep 仍沿 fence 延迟回收。显式 QueueFinish、池释放及必要 CPU 读回的同步保持原语义。
新增 `NVN.TextureFinalize` Litep 区域，便于继续观察销毁期间的锁等待。

GPU 回归增加真实 Vulkan wait 审计：提交 clear 后立即 Finalize 临时 alias，
要求没有 QueueWaitIdle / DeviceWaitIdle / WaitForFences 调用，再在显式完成边界读回原纹理。
旧实现明确失败于 `Texture finalization drained the GPU queue`；新实现在 Metal validation
下通过 364 次提交，包括 256 次 alias 内容保持、呈现复制未完成时的并发销毁、
2,048 个复制像素及 16,384 个字形视图像素。未跳过等待调用伪造通过。

## 修复后的复采

| 指标 | 修复前 | 修复后 |
| --- | ---: | ---: |
| 前台、无 profiler 的约 6 秒 present 计数 | 32.52 次/秒 | 39.70 次/秒 |
| 独立 Litep 首末 marker 间隔 | 196 / 5.98708425 s | 236 / 6.013257542 s |
| Litep 呈现率 | 32.74 次/秒 | 39.25 次/秒 |
| 5.5 秒 CPU 样本中 `vkQueueWaitIdle` inclusive weight | 143 ms | 18 ms |
| 同窗口 `nvnTextureFinalize` inclusive weight | 151 ms | 1 ms |
| Metal System Trace 应用记录数（不是 draw 数） | 14,647 | 3,232 |
| 其中未匹配 GPU / 缺 encoding duration | 13,922 | 2,362 |

两种独立测量都显示本次短窗口呈现吞吐改善约 20%；它不是跨场景平均提升承诺。
Vulkan 尚无独立 Guest frame token，所以使用 present 计数定义，不能声称每次都是
一帧唯一游戏逻辑。原生 Metal 独立前台窗口为 102.11 次 drawable present/秒，
只用作架构效率参考，不与 Vulkan 的 60/120 Hz 节拍差异混成严格 FPS 倍率。

修复后 CPU 捕获共 8,735 样本，583 个缺失完整栈。剩余 18 个含 WaitIdle 的样本
全部同时包含 `nvnMemoryPoolInvalidateMappedRange`，没有纹理 Finalize；必要的 CPU
读回同步未被删掉。`xbg_submit_now` 仍为 2.346 秒 inclusive sample weight，
`MVKCommandEncoder::encode` 为 0.918 秒，`inline_commands` 为 0.641 秒。
吞吐提高意味着同时间窗口处理更多工作，不能单看这些总权重上升就认定回退。

修复后的 GPU channel 区间并集为 Vertex 1.457 s、Fragment 1.247 s、Compute 0.068 s。
并集仍分别计算，未推导“总 GPU 利用率”或 CPU/GPU 同时忙比例。
GPU 表与 CPU 表均完整导出，原始 backdated signpost 和 DWARF 分析警告保留。

新 Litep 文件 SHA-256：
`df1261cacc531cfa79178124e2b51cbadb673edfe2668b24065aedae9fa7da11`。
11,697,797 条事件、237 个 marker、1,073 个完整 chunk，无截断或 scope 配对错误；
一个 GPU zone 边界不完整、21 个 CPU scope 结束边界开放，GPU/CPU alignment 仍 unknown。
独立 SQL 结果与 reader 报告保存在 `litep-before-analysis.json`、`litep-after-analysis.json`，
汇总为 `comparison.json`，不直接读取 Litep 的私有 SQLite。

新插桩显示约 6 秒内 19,644 次 TextureFinalize，经过时间合计约 38 ms（跨线程，不能
当作关键路径），其中包含等锁；不是 19,644 次 GPU 同步。最重的 flush 线程仍在前端锁
等待 2,477 ms，这一瓶颈没有消失。该窗口物理 image 为 712，host objects 为
10,798–10,801；不能据一个窗口宣称无长期泄漏。进程已持续八分钟以上，场景文字仍可见，
没有进程退出；单张检查图不代表所有渲染异常或间歇闪黑已关闭。

## 下一轮架构工作

1. 对齐 MacNative 的命令翻译与编码工作线程：保留不可变命令、uniform 数据和纹理引用，
   把提交线程持有的前端锁与实际翻译生命周期拆开，再考虑 Foundation JobSystem。
   不能直接把可变 Guest 地址交给后台线程。
2. 给 QueueFence / SyncWait 添加提交 token 和锁持有者关联，在有证据的依赖边界上
   只等目标 fence，CPU 等待期间释放前端锁；不能把 QueueWaitSync 简单改成无序并发。
3. 对保守 flush 的命中范围和 dirty/writeback 语义做细分，减少普通上传被全局锁阻塞。
   不可为降低统计数字而删除真实读回、pool 生命周期或跨队列依赖。
4. 再做关闭原生 Metal keepalive、统一节拍的受控对照；当前数据不足以把两后端的
   shader 指令成本或 GPU 利用率直接作等价比较。

本轮尚未达到 CPU 或 GPU 成为唯一瓶颈的目标，没有把等待移到另一线程就宣称优化完成。

## 构建与运行状态

Mac GPU 回归及 Mac Vulkan 构建通过。Android BOTW Debug APK 与独立 probe 交叉构建通过；
本轮 `adb devices -l` 为空，没有安装、没有 Android 实机性能结论。
APK：`/Users/bytedance/workspace/native-switch-study/macos-base-20261025/botw-133-finalize.apk`，
SHA-256 `4064fbb817a8137dbecdf424627ab1878ab9884166ef9dc505af2d7189a89003`。
Mac 保留修复版 PID 55639 供观察；Litep capture 已结束、resident ring 已关闭，
GPU capture / Metal validation 关闭。测试前后的存档及原应用均保留。
