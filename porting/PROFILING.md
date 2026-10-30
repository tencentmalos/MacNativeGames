# 原生移植底座的 Litep 采集

调度优化的同进程 A/B 使用 `renderer_schedule status`，以及
`adaptive/present/invalidate/reads on|off`、`batch 0..65536`。`reads` 控制仅有计数器报告
时的缓冲读取过滤，保留真实写回依赖。请求仅尝试取得 engine 锁；
忙时返回可重试状态，不等待翻译或 GPU。默认启用三项优化、批次 1024；
`present_capable` 单独说明宿主是否已接入呈现源转换。参数不写入启动设置。
原因 scope `NVN.Fence.RingReuse/Complete/...` 与实际 submission ID 用于区分等待；
前端锁外层的 NVN API scope 表示调用归属。实验与限制见
[Fence 与全局锁分析](../macos/FENCE_LOCK_20261026.md)。
跑图慢帧、虚拟核等待和预编译数据归档的后续实测见
[复苏神庙跑图分析](../macos/ROUTE_HITCH_20261026.md)。

## 有界跑图与 Flush 诊断

Mac Metal、Mac Vulkan 和 Android 共用 `input_pulse` 控制入口：
`status`、`stop`，或 `forward/backward/left/right/look_left/look_right 1..3000`（毫秒）。
输入在 Guest 的 HID 读取路径中生效，到期自动释放；真实键盘、手柄输入和输入清理会取消。
已有脉冲仍有效或锁忙时立即返回可重试状态，不叠加移动。状态中的 `applied_generation`
表示 HID 已实际读取该脉冲；它不是人物位置证明。Litep 的 `Input.Pulse.Applied/Released`
给出实际读取起止标记，`Generation/Direction/DurationMs` 给出请求参数。

复苏神庙可从同一存档读入，稳定后先采静止基线，再开始 6 秒、256 MiB 文件采集，
约 1 秒后发送 `input_pulse forward 1200`，结束时检查 `active=0`。
更换后端或开关做路线 A/B 时重新读取同一存档，不把往回走当作准确复位；
保留存档哈希、输入参数、实际采集 ID 和 PID。采集期间不要并行编译或索引旧采集。
用 Litep 原始 `sdk_frame` 排慢帧并比较连续邻帧；上屏完成标记不等于独立 Guest 帧身份。

Mac 可使用 `python3 macos/route_capture.py --pid PID --output NEW_MANIFEST.json` 自动执行
一次有界片段。工具拒绝覆盖清单、拒绝已有录制或输入脉冲，并要求采集 ID 不变、
HID 已消费输入、文件状态为 ready；异常时只清理它自己创建的脉冲和采集。
清单保存生产者的原文件路径和 ID，随后用 Litep `collect_capture` 校验、收集与分析。
该工具不判断角色位置，也不自动存档。

调度排查增加 `Sync.CoreSchedulerLock`（获取调度器锁）和 `Sync.VirtualCore.Wait`
（正常宿主调用等待虚拟核）两个范围。`Core.Wait.*` 在录制开启且确实没有空闲允许核时
记录等待线程的 Guest handle、mask、priority，以及进入等待时各核的 owner、持有时长和
最近 SVC。`Granted` 是返回时分配到的核；配合 `threads` 的 handle→host_tid 快照分析。
owner 可能在等待中变化，入口快照不是完整通知链；不能仅按时间重叠认定阻塞关系。
信号抢占路径仍直接调用无 profiler 的调度函数，避免在 signal handler 中记录。

`MNG_FLUSH_DIAGNOSTICS=1` 启动后记录最近 32 个 Flush 范围和调用次数，
`renderer_flush` 用只尝试获取的 engine 锁查询这些范围与当前物理图像、写回和报告的交集。
此查询是当前资源状态，不能替代调用当时的依赖证明。该诊断默认关闭；原有保守 Flush
顺序仍保留，不能因当前查不到图像就绕过写回或别名资源的生命周期约束。

底座直接使用固定版本 Foundation 的 `profiler_ring` 与
`RingVulkanGpuBridge`，CPU 和 GPU 共用同一个 profiler_sdk 实例。
默认关闭记录；基础 CPU 范围包括 NVN draw/dispatch、提交、围栏、CPU 写入失效处理，
以及宿主 present、queue idle 和节流。`profiler_detail start` 才打开纹理、描述符、
快照等高频细分范围。细分采样会改变帧耗时，不宜直接当作无插桩性能。

GPU 使用异步 timestamp query，范围为 `NVN.Submission`、`NVN.UploadPrologue`
和 `Host.Present`。后者仅包含最终 blit/HUD，并非整个游戏 GPU 帧。
查询由渲染线程非阻塞回收，Android Service 只读取缓存状态；不要从 Service.dump
等待渲染、驱动或 guest 会话锁。当前使用 Foundation 的 fence midpoint 校准，
避免把尚未验证的驱动扩展时钟关系当作对齐依据。

## 控制与进程身份

调试包的服务是 `org.macnative.android.PortDebugService`，运行在 `:game`
进程并要求 Android `DUMP` 权限。Release manifest 不导出该服务。
启动器与游戏 PID 不同，不能只用 `pidof org.macnative.android.botw`。

```sh
python3 porting/tools/profiler.py --serial DEVICE --package org.macnative.android.botw \
  --expected-pid GAME_PID control profiler_ring status
python3 porting/tools/profiler.py --serial DEVICE --package org.macnative.android.botw \
  --expected-pid GAME_PID control profiler_ring start
python3 porting/tools/profiler.py --serial DEVICE --package org.macnative.android.botw \
  --expected-pid GAME_PID control profiler_ring stop
python3 porting/tools/profiler.py --serial DEVICE --package org.macnative.android.botw \
  --expected-pid GAME_PID control profiler_ring dump 60
```

Ring 每个记录线程为 8 MiB；这是容量，不是保留秒数。等待返回的 `dump_id`
达到 `ready` 后，用同一 PID、ID 收集：

```sh
python3 porting/tools/profiler.py --serial DEVICE --package org.macnative.android.botw \
  --expected-pid GAME_PID collect --mode ring --id DUMP_ID --output LOCAL_DIRECTORY
```

连续文件可通过 `control profiler_capture file 128 8` 启动，参数为 MiB 和秒。
查询 `profiler_capture status`，同一 ID 到达 `ready` 后用
`collect --mode file --id CAPTURE_ID`。允许 `profiler_capture stop` 提前结束。
失败文件保留在设备，不删除前缀；正常收集拒绝失败、未完成或身份变化的产物。
工具同时验证 Service PID、生产者 PID、文件长度和设备/主机 SHA-256，
保留 SDK 原始 sidecar，并另存 `.identity.json`。不会覆盖旧产物。

## 解释边界

- `Native.Present.Token` 和 FrameMark 来自成功的宿主 present；没有独立 Guest 帧编号。
- CPU scope 是经过时间，包含等待，不等于 on-CPU 时间。
- GPU query 是提交范围耗时，不能把重叠区间直接求和称作整帧耗时。
- sidecar 中 `gpu_cpu_alignment=unknown` 必须保留，直到该次捕获的 context
  校准关系通过验证；状态命令显示支持时间戳并不能证明捕获的所有 context 可用。
- 启停记录可能留下边界 scope/GPU pair。文件有 footer 不代表无丢块。
- 重复模式切换和 ring 滚动窗口的旧 SDK 问题仍需单独验证；用于性能结论的捕获
  必须检查 decoder diagnostics、chunks_skipped、完整帧范围和实际场景。
- `Native.Pipelines`、`Native.ShaderLoads`、`Native.UploadBytes` 等是相邻成功
  present 间的增量。不要仅凭低帧率推断存在动态 shader 编译。

使用 Litep MCP `open_trace` 或 `index_trace` 解码原始 PROF；大文件优先磁盘索引。
只比较相同存档、镜头、驱动、预热与采样设置的连续完整窗口。没有 scheduler/KGSL
证据时，不把未标注的 CPU 空隙称作空闲，也不把 GPU query 的空隙称作 GPU idle。

## Odin 3 实测状态（2026-10-24）

APK 76 在复苏神庙静止镜头连续文件采集 8 秒，37 个 chunk 全部解码、无丢块，
59 个完整成功 present 窗口平均 134.865 ms，P50 131.206 ms。
CPU 主线程平均 Draw 32.328 ms、Fence 17.981 ms、ImageIndex.Rebuild 13.421 ms；
未插桩部分为 61.689 ms，不能把它归因为 Guest 执行或调度等待。
60 次 counter 样本中，新增 pipeline、shader 加载及 pipeline 编译耗时均为零。
该静止场景低帧率没有指向持续动态编译；加载或换场景仍须单独测量。

同次 GPU 记录恢复 1783 对完整区间；有 1 条不完整 GPU 区间诊断。
使用 fence midpoint 后时间落入 CPU 捕获窗口附近，但精确误差和跨时钟因果
尚未验证，仍保留 `gpu_cpu_alignment=unknown`。同进程第二次 5 秒文件捕获
23 个 chunk 无丢块，GPU context 2 可解析，保留 2 个边界 orphan time。

Ring 导出虽返回 ready，原始文件仍出现 `unknown event tag 0x00`，不能用于
性能结论。当前优先使用有边界的连续文件；不将 SDK 的导出完成状态等同于
解码通过，也不把旧格式缺陷标为已修复。原始 PROF、SDK sidecar、PID 和 SHA
收集记录在本机 `porting-20261024-gameplay/litep76-{scene,repeat,ring}` 下保留。

## 统一 debugbus 与细分探针（APK 79）

JNI 现在转发到 Foundation `DebugCommandRegistry`。注册表在首次使用时固定，
生命周期属于底座；Android Service 只是受 DUMP 权限保护的传输入口。
`help`、`debug_status`、`renderer_status`、`guest_modules`、`threads` 和全部
profiler 命令共用这个入口。`debug_status` 的配置身份明确标作 expected；
实际加载模块的 Build ID、SHA、映射和 patch 收据以 `guest_modules` 为准。

`renderer_status` 是成功 present 后缓存的统计，包含来源、样本年龄和 GPU
统计所对应的较早 frame，不能把不同 frame 的 CPU/GPU 值相减。
`threads` 最多复制 256 个线程的 handle、host TID、虚拟核、优先级和等待类别。
它只尝试获取线程表锁，失败返回 `emulation_session_busy` / `retryable=1`；
不暂停线程、不访问 Guest 内存、不在 Service 中查询 GPU。

基础 CPU 桩点新增 `HLE.SVC.0xNN`、SVC 诊断记账、Guest 回调、Guest 锁/
条件/地址/同步对象等待、虚拟核重新获取和协作式抢占检查。
SVC 原始编号避免将猜测的 Guest 符号当作事实。
**异步 SIGUSR2 抢占处理器中不调用 profiler**，其耗时仍属于未知覆盖。

```sh
# HLE 导入调用名称来自运行时实际注册的导入名，不是推断的 Guest 地址。
python3 porting/tools/profiler.py --serial DEVICE --package org.macnative.android.botw \
  --expected-pid GAME_PID control profiler_hle start
# 每 present 最多选择 256 个 GPU 命令；draw 每 16 次选一次。
python3 porting/tools/profiler.py --serial DEVICE --package org.macnative.android.botw \
  --expected-pid GAME_PID control profiler_gpu start 256 16
```

`profiler_hle`、`profiler_gpu`、`profiler_detail` 独立开关，默认关闭；
采集完用各自的 `stop` 恢复。文件捕获完成只恢复 ring 状态，不擅自改变这些开关。
`profiler_coverage` 列出已接入的范围和仍未知的部分。

GPU 细分直接包围 32 处 `vkCmdDraw*`、`vkCmdDispatch`、copy、clear、blit
调用，实际生成 timestamp pair；不是根据 CPU 耗时估算。
`GPU.Detail.Observed/Selected/SampledOut/Rejected`、Budget、DrawStride
随每次成功 present 记录，明确预算耗尽、采样和 query 容量丢弃。
预算最大 1024，draw 间隔最大 1024。查询回收/重置仍在 render pass 外。
粗粒度 submission/upload/present 不受细分预算影响。
`Host.Pipeline.Compile` 仅包围实际 Vulkan pipeline 创建 API；
`NVN.Pipeline` 仍然是查找/绑定范围，不等于编译。

分析文件时必须排除跨录制停止/重启边界的旧 frame marker 区间。
例如 APK 78 的第二次基础捕获虽然 decoder 将首区间标为 valid，SDK frame 65
实际跨越约 286 秒的未录制间隔；使用连续 frames 66–125 后，60 帧均值才是
130.544 ms，P50 126.586 ms，而不是把停录时间算成游戏卡顿。

## 资源生命周期观测

`renderer_resources` 返回非阻塞的当前保留量及纹理累计初始化/销毁次数。它与 `renderer_status` 配合，用来区分运行时间增长和真实同时存活的资源。Litep 中对应的 `Native.HostObjects`、`Native.TextureSnapshots`、`Native.ImageIdentities`、`Native.PhysicalImages`、`Native.CommandBytes` 和 `Native.TextureInitializations` 是当前值/累计值，不能相加当作内存占用。查询忙时重试，不等待 GPU 或 Guest 锁。优化迭代流程见 [OPTIMIZATION.md](OPTIMIZATION.md)。


`renderer_targets` 返回最近一次成功呈现前记录的 render pass 尺寸、采样数、颜色/深度附件数量，以及呈现纹理和 crop 尺寸。最多 128 个不同组合；`omitted_passes` 必须与结果一起检查。`target_0` 等编号字段由采集工具完整保留。纹理分配尺寸、有效 crop、游戏内部动态分辨率、Android Surface 是四个不同概念，不能只报其中一个而统称“当前分辨率”。

`system_mode` 返回不可变启动配置及 Guest OE 查询次数：720p（手持模式）对应 operation/performance 0/0，1080p（底座模式）对应 1/1。`operation_queries` / `performance_queries` 为 0 时不能把配置当作该接口已被 Guest 实际读取的证据。Service 只读已初始化的配置与原子计数，不启动初始化或等待 Guest 锁。与 `guest_modules` 的补丁收据和 `renderer_targets` 的 crop/附件尺寸一起验收模式；普通用户默认手持 720p、1×。

开发版可在重启前设置 `adb -s DEVICE shell setprop debug.mng.read_preflight 0`，单独验证 D2R 的按 CPU 访问范围同步路径；1 为默认保守路径。采集身份含该开关，最终必须恢复已验证配置。`debug.mng.input_log=1` 只启用有界 HID 样式/控制器调用日志，结束后改回 0。不要将诊断开关加入普通用户启动界面。

## 首屏节拍诊断（2026-10-25）

`renderer_status` 增加 `present_interval`；Litep 中相同信息为 `Native.PresentInterval`。
它是游戏传给 NVN 的值，不是强制 FPS 选项。`Host.WindowAcquire.Pacing` 记录逐图像复用等待；
`Host.Present` 下分别记录 `AcquireImage`、`Overlay`、`Submit`、`QueuePresent`、`WaitIdle`。
这样可区分移植层人为等待和 Android WSI 的实际阻塞，仍须比较完整帧，不能只比较某个 scope。

`renderer_targets` 同时包含 Guest GPU 时钟映射的方式、GPU/CPU anchor 和完整采样误差区间
（`guest_clock_deviation_ns`）。`Host.GpuClock.Calibrate` 显示周期校准成本。
这组信息服务于 NVN counter 的 Guest 语义；不替代 Litep GPU context 的校准验收。
首屏实测与原始文件身份见 [TITLE_PERFORMANCE.md](TITLE_PERFORMANCE.md)。

### 首屏显示节拍实验

`Native.DisplayPaced` 表示本次 Acquire 是否使用 Android 显示回调，`Host.WindowAcquire.DisplayTick` 记录该等待。`Host.WindowAcquire.Pacing` 是回调停顿、无窗口或显示频率不能整除 Guest 节拍时的单调时钟后备。回调丢拍按真实帧时间推进计数，落后的帧立即开始，不再额外等一个周期。

进程启动前可用 `debug.mng.display_pace=1` 实验性开启显示回调节拍（默认关闭；长采样尚无收益），用 `debug.mng.present_mode=mailbox` 请求宿主 Mailbox（默认 FIFO）；两者独立，测试必须记录实际模式、APK/PID 和 `Native.DisplayPaced`，不能只依据属性值。

Android BOTW 默认启用有界呈现线程，可用启动属性 `debug.mng.present_worker=0` 做同步对照。该路径只异步交接 Present，NVN 命令翻译仍同步。`Host.Present.Handoff` 为交接调用，实际成功 present 边界改由 `mng-present` 线程发出；必须以该线程完整帧的墙钟间隔为依据，再对齐其他 Guest/NVN 线程的范围，不能把该线程的未标注部分称为 CPU 空闲，也不能丢掉调用线程的 Draw/Sync 成本。


## Guest 地址等待与图像 Flush 对照（2026-10-27）

`profiler_address start|stop|status` 独立控制 SVC 0x34/0x35 的地址等待标签，默认关闭。
它记录实际挂入等待队列的 `Enqueue`、选中目标的 `SelectedNotify`、`Wake` 和重新取得
虚拟核后的 `Resume`。每个事件携带 `Epoch`、`WaitId`、`Object`、`Waiter`、`Peer`、`Result`。
`Peer` 只有选中通知事件填入真实通知者的 Guest handle；宿主 TID 来自事件所在轨道。
超时没有虚构的通知事件，录制代际变化后的旧 token 不再输出事件。
按 epoch + wait ID + 对象 + waiter 关联，不能按事件顺序或时间重叠补造依赖。
`Wait` 包含重新取得虚拟核的时间，通知到恢复之间不等于纯宿主调度延迟。

`renderer_flush_image status|on|off` 控制共享 Vulkan 的仅图像范围 Flush 路径。
它独立于 `renderer_schedule flush`，后者管理活动范围检查。普通池、同步翻译、活动范围
检查开启，且范围只与独立 VkImage 内容所有权相交时，执行 CPU 发布屏障，不再等待
翻译全局锁。写回、report、event、未知所有权、物理／虚拟别名和有序翻译保留原处理。
`flush_image_fast` 是累计调用数，`NVN.Flush.ImagePublish` 是对应 Litep scope。
不修改 Invalidate、GPU fence、写回和资源退休。它不是完整的 CPU/GPU 纹理内容合并机制。

`android/tools/benchmark_image_flush.py` 在指定 PID 内按 off/on/off/on 测量；
无记录的呈现计数用于 FPS，随后另采 Guest tags + 地址等待的 Litep 文件。
脚本核验模块与 APK 身份、前台、预热工作负载和其余渲染配置，结束恢复开关并关闭采样。
实际收益及限制以实机报告为准。
