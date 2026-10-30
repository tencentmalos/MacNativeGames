# Mac / Android 原生 NVN 的并发边界与 Foundation JobSystem

## 结论与当前状态

可以复用 Foundation JobSystem，但应先拆清输入所有权和依赖，再调度任务。本轮已将对象查询、命令录制和 Vulkan 翻译拆开锁域，Submit 保留不可变命令及纹理引用，翻译期间允许其它 Guest 线程继续录制。**2026-10-26 已接入真实 Foundation WORK_JOB，用于有界、按 key 去重的 PSO 预热；Vulkan Draw 翻译仍同步。**呈现交接线程与该任务池是不同的执行通道。实现与实际并发证据见 [PSO 预热](nvn_vulkan/PSO_WARMUP_20261026.md)。

Odin3 场景基础采集的 Draw 约 25–26 ms、Fence 约 16 ms、GuestCondition 约 24 ms/帧。短时详细采集还定位到 ABI 常量查询和映射范围 Flush 被整次翻译阻塞。稳定场景中 pipeline/shader 新建计数为零；把编译搬到 worker 不是这段低帧率的解决方案。详细插桩显著增加开销，其经过时间只用于定位，不替代基础采集的 FPS。

## 查阅的实现

| 来源 | 固定身份 | 可复用的设计 |
| --- | --- | --- |
| MacNativeGames | `a02228e8ec884dae66827de9a593bb9d9a31f0e5` | `games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c` 的 EndRecording、BOTW control-handle、tex_pin、QueueSubmitCommands、x_push / x_drain；异步提交前保存 UBO、解析延迟绑定，使用提交票据关联实际 GPU fence |
| Azahar 本地源码 | `e2b0781aad09e860a156a44d190943a54a7aa7a9` | `src/core/core.cpp` 建立工作池与外部线程槽；`src/video_core/gpu.cpp` 将不可变 PICA packet 解码交给 WORK_JOB，ticket 同时保留结果、异常和 waiter；有副作用的寄存器应用仍顺序消费 |
| Azahar 渲染服务 | 同上 | `vk_pipeline_cache.cpp` 使用有并发数、任务数、输入字节上限的 CompileQueue；`scene_lift/lift_runtime.cpp` 用 strand 保持共享状态的顺序 |
| 本移植固定的 Foundation | `2f27d3bb41c1659d33b89905255fab2675fc0144` | 已有 `basic/async/async_task` 的 JobSystem、waiter、strand、external slot；`engine/resource/JobSystemExecutor.h` 说明工作池与拥有渲染上下文的线程之间的交接 |

Azahar 的本地 Foundation checkout HEAD 是 `9c6fcb5397fa7547bf31a0d3edaf947499aae1ad`（父仓库将该 submodule 标记为已修改），与本项目不同，不能直接移动 gitlink 或假设依赖相同。本轮只读其实现；未复制 GPL 的模拟器代码，也未修改 Foundation。Mac 的可选异步 replay 仍是有序消费者，并非多个线程同时改写 Metal 状态。其 async 模式还禁用了一种 control-memory token 回收，不能照搬成已经闭合的 Android 生命周期方案。

## 已落地的最小并发契约

- `object_mutex` 只管理宿主对象表；只读映射地址查询不等待翻译。
- `command_mutex` 保护录制器、控制句柄、命令副本及程序快照。状态绑定辅助函数也使用此锁。这里不得调用会获取 engine 锁的操作。
- Submit 在短锁区间把所有输入句柄解析为 `shared_ptr<CommandCopy>`，离开 command 锁后进入同步翻译。控制位置被重录或 Finalize 不会改变提交已经选定的字节和纹理引用。
- 解释器使用独立 CPU 命令范围表检查完整读取范围，每次 Submit 重置其区间缓存。命令副本不再作为 GPU 内存池注册或注销。
- 图像、描述符、内存池变更及 GPU 完成语义仍走原顺序；纹理物理存储继续随真实提交槽退休。
- 固定 ABI 属性直接返回。可能阻塞的 Flush/Invalidate 在等待前归还 Guest 虚拟核心，释放宿主锁后才重获核心。
- Mac / Android 的 Present worker 现在分三段执行：准备与队列提交持 engine 锁，等待本次复制 fence 时释放该锁，资源退休再持锁。整个任务的闭包和源 Image 保留到退休；后续翻译可以在复制等待期间推进。所有 VkQueue 调用仍由 engine 锁序列化。CPU `nvnSyncWait` 另以短锁保存 sync 快照，目标 fence 的实际 GPU 等待不持 front/engine 锁，退休和写回保持原有顺序；回归验证独立提交能够推进。`nvnQueueWaitSync`、提交槽压力、同步翻译及 CPU 可见写回仍需各自的顺序契约，不能统称为已异步。
- 呈现前使用已验证的纹理范围预检，不再无条件兑现所有无关写回/报告；真正相交的 CPU 读取继续同步。present 完成只表示源纹理复制完成及 QueuePresent 已成功接受，不代表屏幕扫描完成。
- GPU detail accounting 在锁内取快照并归零，完成线程仅发 frame marker 和发布快照，避免与下一批 draw 的非原子计数发生数据竞争。

Flush 的范围相关可见性需要继续单独对齐：Mac `mtl_pool_invalidate` 会处理相交的镜像草地遮罩并避免覆盖较新的 CPU 写入。Android 本轮保留原有翻译顺序，不能简单去掉锁、只保留 CPU fence 后就宣布语义等价；也不能随意增加整队 Finish。

2026-10-26 后续已去掉两类不必要的等待：普通内存池 Invalidate 只兑现相交的写回、报告与事件，物理/虚拟别名保守回退；呈现源转换放入宿主已有的呈现命令缓冲，无须为一条 barrier 复用 NVN arena。自适应拆批在下一槽忙时继续翻译，仍受两槽和 arena 上限约束。全局锁没有被无条件移除，真正的 CPU/GPU 依赖仍由实际 fence 发布。实现、同进程 A/B 与合成并发回归见 [Fence 与全局锁分析](../macos/FENCE_LOCK_20261026.md)。

## JobSystem 的接入顺序

后续同场景证据见 [Mac 并发与 pass 分析](../macos/CONCURRENCY_20261025.md)：实际场景中的 CPU SyncWait 已是完成快路径，6 秒内总计约 0.096 ms。主要工作仍在约 10.75 ms/提交的 NVN 翻译、Flush 等待与过多的附件反馈 pass。先压缩不必要的工作并建立正确依赖，再决定 JobSystem 的准备任务粒度。

2026-10-25 Mac Vulkan 基础 Litep 采集补充了相同底座的等待证据：6.04 秒窗口内，呈现线程 `NVN.Fence` 经过时间 1.535 秒，`Host.Present.WaitIdle` 仅 85.50 毫秒；多个 Guest 线程出现 1–2 秒前端锁等待。聚合不能确定锁持有者，CPU/GPU 时钟对齐也尚未验证。进入调度改造前，应为前端锁持有者、Fence 和翻译批次增加同一 submission token，区分等待 GPU、等待翻译和等待 Guest 核心；不要先扩大线程池或只移走 WaitIdle。证据与完整性限制见 `macos/VALIDATION.md`。Mac 原生 Metal 的 encode worker 使用 WFE/SEV 和 semaphore，Instruments 在该函数上的采样占比不能直接解释为忙循环浪费。

1. **已完成 PSO 准备阶段**：`porting/jobs` 链接当前固定 Foundation 的真实 `foundation_async_task`、core、reflection、allocator 和 property 目标。`KeyedJobs` 只封装任务去重、输入预算、领取与取消，不自建工作线程池；PSO 会话退出先关闭任务，再释放 Vulkan 依赖。其他准备任务尚未接入。
2. 提交票据必须拥有不可变命令、引用纹理代、程序代、描述符版本、延迟绑定值和实际会被 CPU 读取的 UBO/间接参数快照。仅复制命令中的 Guest 指针不够。快照必须在 API 约定的提交边界取值，不能让 worker 晚读下一帧数据。
3. 将无副作用的命令分析、资源范围归并、已保留字节的转换或散列交给 WORK_JOB。按字节/命令批量调度，避免每个 draw、每个 NVN API 或每个 memcpy 发一个任务；使用有界队列、内存预算、取消代号和失败结果。
4. 用有序 strand 或拥有 Vulkan 状态的 external slot 消费准备结果。State、command pool、descriptor pool、图像布局和同一 VkQueue 仍按依赖串行；GPU -> CPU 读回不能作为普通后台工作随意延迟。
5. FenceSync 区分“准备完成”“翻译完成”“GPU 完成”三个阶段。Wait/Flush/资源销毁绑定对应票据，等待前释放 Guest 虚拟核心，禁止 worker 等待必须在它占有的唯一槽执行的子任务。
6. Stop 先关闭提交入口，取消尚未开始的准备任务，再等待在途任务与 renderer/GPU 所有权退休，最后销毁模块映射、Vulkan 和 JobSystem。debugbus 始终只读缓存状态或 try-lock。

## 验收

每个阶段提供串行开关与相同输入的结果比较；先验证命令和资源代际一致、Finalize/控制内存复用、关闭时无悬挂，再测帧吞吐和输入延迟。Litep 增加准备排队/执行、结果等待、翻译和 GPU 票据，以稳定 submission token 串联。完整帧预算不得把跨线程重叠相加，也不能把等待转移当作收益。独立准备任务没有被测得的占比之前，不将增加工作线程本身视为优化。
