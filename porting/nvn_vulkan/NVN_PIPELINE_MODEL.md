# NVN 状态模型与 Vulkan 缓存边界

## 是否有完整 PSO

从本地 NVN 接口和两个独立 native 后端看，NVN 有 `NVNprogram`，通过
`ProgramSetShaders` 提交着色器集合，通过 `CommandBufferBindProgram` 按 stage mask
绑定。它没有把 shader、顶点输入、混合、深度模板、多重采样和附件格式全部合成一个
类似 `VkPipeline` 的公开 graphics PSO 对象。Blend、DepthStencil、Polygon、
VertexAttrib、VertexStream 等状态对象独立绑定；资源绑定和 render target 也独立。

因此一个 Program 不能直接一对一映射到 Vulkan graphics pipeline。正确的桥接需要
从命令流累积的状态生成组合管线；保留有序状态是语义要求，反复重建相同宿主描述则
是可以优化的实现成本。不能将这一点简单描述成 NVN 本身等同于 GLES。

接口证据：Citron 只读 `references/nnheaders/include/nvn/nvn.h` 的
`PFNNVNPROGRAMSETSHADERSPROC`、`PFNNVNCOMMANDBUFFERBINDPROGRAMPROC` 和各类
`PFNNVNCOMMANDBUFFERBIND*STATEPROC`。这些用于接口语义，不作为游戏地址证据。
本工程的 `nvn_vulkan.cpp` 与 `state_api.inc` 也分别录制 Program 和按值状态。

## 当前 BOTW 证据

Odin3 同一稳定存档窗口每帧约 3650 Draw：

| 缓存 | 平均每帧 |
|---|---:|
| 相邻 PSO memo 命中 | 2928.0 |
| 相邻 PSO memo 未命中 | 722.1 |
| 次级 pipeline fast 命中 | 722.1 |
| 新建／淘汰 Pipeline | 0 / 0 |
| Descriptor memo 命中／未命中 | 3604.2 / 45.9 |
| 新建 framebuffer | 0 |

数据来自 `android-translate-20261026/uma-ab-3/opposite/native-full.log`。
当前没有证据支持“稳定场景持续重建大量 VkPipeline”。更明确的问题是**缓存命中太晚**：
描述符 key 仍逐 Draw 扫描 metadata 和绑定，PSO memo 检查前仍组装 Vulkan create info；
缓存存在、命中率高，不等于命中路径足够便宜。线程 CPU 计时确认同步翻译主要在执行，
不是约 25 ms 都在睡眠。详见 [实测记录](ANDROID_TRANSLATION_20261026.md)。

Mac Metal 的 `state_op` 仅在值变化时标记 `g_pso_dirty/g_dss_dirty/args_dirty`；
Draw 只在 Program、pass 或相关状态变化时调用 `pso_get`，其余直接复用当前 pipeline。
这是可借鉴的结构，不需要照搬 Metal 的动态状态集合。Vulkan 是否动态取决于实际启用的
core/extension feature；没有启用的状态必须进入管线 key。

## 第一轮调整

共享 Vulkan 增加 `pipeline_early`（`MNG_PIPELINE_EARLY`），运行时命令为
`renderer_schedule pipeline on/off`。资源与 Pass 先正常解析，然后用现有完整 memo key
检查管线；命中时省去 Vulkan fixed-function create info 和布局 plan 的重复组装。

- 保留 shader variant、pipeline layout、render pass、topology、sample count、
  固定状态等原有完整身份。暂不把原 key 中的动态字段进一步归一化。
- 顶点 plan 必须匹配当前状态；不能只检查 valid。某次中间 Draw 若因异步管线未就绪
  而跳过，可能留下不同的顶点 plan，而旧 PSO memo 仍有效。
- 保留资源检查、uniform 快照、动态 offset、GPU 写回、Fence 与 LRU touch。
- capture 模式走完整准备路径，保留属性与资源诊断输出。
- `NVN.Pipeline.EarlyHits/StateBuilds` 为累计计数，每批进入 Litep；
  `NVN.Draw.PipelinePrepare` 在选中的 detail Draw 中包含完整准备成本。
- `benchmark_translation.py --pipeline-only` 进行同进程关／开／关／开，FPS 与
  profiler 窗口分开；不得只凭缓存命中次数宣称帧率改善。

进一步的候选是按实际值变化更新 state generation、稳定的 descriptor layout / binding
plan，以及将 descriptor 身份与每次 draw 的动态地址分离。资源内容或生命周期变化
不能因绑定对象相同而跳过；generation 的失效面需要独立测试，再逐项 A/B。

## 首轮同进程结果与整机利用率

最终 APK SHA：`a2ac1f0ee26db7f913fc4bd529f860610a4f847d83fdc1f0f87461d7b083a09c`，
Odin3 PID 17101，同原存档自动加载后进行四个 20 秒窗口：

| 提前命中 | 关闭 A | 开启 A | 关闭 B | 开启 B |
|---|---:|---:|---:|---:|
| FPS（profiler 关闭） | 22.574 | 22.462 | 22.379 | 22.154 |

没有可宣称的帧率提升。独立基础 Litep 的第一对窗口中，完整批次的线程 CPU 均值
由 26.926 ms（68 个批次）到 25.572 ms（71 个批次）；经过时间由 27.158 ms 到
25.841 ms。另一次 1/32 detail 中 PipelinePrepare 由 1.102 μs 到 0.713 μs/选中
Draw。两种测量支持准备工作有所减少，但不同采样窗口、探针开销与运行漂移仍存在，
不能把它包装成整体 FPS 增益。Descriptor sampled scope 仍约 6.1–6.2 μs，包含嵌套
Snapshot 等探针开销和部分非 Draw 调用，不能直接乘上全部 Draw 数推导真实成本。

两组基础和两组 detail 文件均完整，无丢块或 CPU 配对错误，仅 GPU 边界区间不完整；
跨 CPU/GPU 时钟对齐仍未知。根目录为 `android-translate-20261026/pipeline-ab-1/`
及 `pipeline-detail/`。关闭／开启基础文件 SHA 分别为
`cd39b30ded22f90980453546eca2696c899b14154eac81e062db276ef9109ed8`、
`ec237d6720e4698b6b85cc16bddcaed9ae89d269c5f5b7f080dcc57851e6dfd9`。

GPU probe 在两个模式下均通过：Android 395 次提交、Mac 392 次。开启模式分别有
96 次提前命中，完整状态构建降到 65 / 62；关闭时为 161 / 158。动态 UBO 更新、
绑定变化、回读像素、slot 复用、管线 LRU、对象生命周期沿用并通过现有实际 GPU 验证。
这不代替实际游戏完整画面检查。

### 为什么总占用低但翻译线程持续忙

关闭 profiler 后读取同一 PID 的 `/proc/.../task/*/stat` 和 `schedstat`，12.793 秒：

- 按设备 8 个在线逻辑核等权归一化，总 CPU 为 **24.39%**（未做频率／核能力加权）。
- 翻译线程 TID 17318 为 **60.81% 的单核时间**；实际运行 7769 ms，runqueue 等待
  30.94 ms。这个线程没有被 OS 长时间饿死；单次 Translate 内约 99% 在执行，
  不等于整帧或整颗 CPU 99% 利用率。
- 其余四个较忙 Guest 线程分别约占单核 22.5–23.9%；不能把它们的等待时间相加。
- 设备 KGSL `gpubusy` 的十个读取窗口加权比例为 **28.45%**。这是设备级指标，
  不是按进程归属的 GPU busy，也没有和 Litep GPU query 建立跨时钟因果关系。
- Litep 的同一翻译线程 3 秒窗口内持有 `front_mutex` 约 1.878 秒；其他线程的
  FrontLock.Wait 平均约 8–18 ms。`nvnQueueSubmitCommands` 源码仍在 `LOCK` 内
  同步执行 `xbg_submit`，仅 command recording 使用独立锁。

因此当前不是 CPU/GPU 整体算力用尽；是桥接单线程处理、全局资源锁与 Guest 生产
之间的串行关键路径。后续应同时减少 descriptor 命中路径的 metadata 扫描、按实际
值变化更新 generation，并把可证明独立的资源生命周期／准备操作从全局锁拆出。
不能仅增加 encoder worker 数或删除 Fence 来制造更高占用。

原始逐线程快照、采样跨度和 GPU 数值保留在 `thread-utilization/`；保留线程身份按
TID 与 start ticks 匹配，已退出线程不进入统计，整机 CPU 数字存在 jiffy 与 ADB 读取跨度误差。

## 并发预热与有序消费

2026-10-26 已将原来的独立预热线程替换为 Foundation WORK_JOB 和有界 KeyedJobs。
同一个已排队 key 在运行前可被前台领取，已在运行则等待现有结果，避免前后台重复编译。
保留完整状态 key、当前资源与 descriptor 处理以及原始 Draw 顺序。该阶段尚未把每帧的
资源准备／状态聚合移到 worker，不能称为全部 Draw 已并行。

原 D2R 实现本来已有预热线程（Odin3 默认两个），所以“新增并发”不是准确的基线描述。
本次新增的是共用 JobSystem、任务去重和结果交接，以及实际并发证据；
[测试记录](PSO_WARMUP_20261026.md) 单独列出 worker 数比较与稳定帧率。
