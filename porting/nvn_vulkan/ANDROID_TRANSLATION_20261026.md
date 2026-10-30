# Android NVN 翻译开销与统一内存检查（2026-10-26）

## 测量口径

这里的 `NVN.Translate` 是同步执行一批 Guest 命令的**经过时间**，并非 Vulkan 驱动
的 CPU 执行时间。`NVN.Draw` 包含描述符准备、Guest 缓冲快照、纹理处理、Pass/
管线查找、状态校验、命令录制和写回登记。不能把它解释成 `vkCmdDraw` 的成本，
也不能由 Litep scope 判断 CPU 正在执行还是被抢占。

初始完整采样（PID 9220，96 个完整 Translate，约 4 秒）：

| 范围 | 次数 | 平均 | P50 | P90 | P99 | 最慢 1% 占累计耗时 |
|---|---:|---:|---:|---:|---:|---:|
| NVN.Translate | 96 | 25.025 ms | 25.938 ms | 30.473 ms | 37.063 ms | 1.72% |
| NVN.Draw | 355392 | 4.923 μs | 2.917 μs | 9.688 μs | 23.646 μs | 13.69% |
| NVN.CpuRead.Preflight | 356566 | 0.961 μs | 0.834 μs | 1.511 μs | 3.020 μs | 5.29% |

分位数来自完整区间排序，P99 使用 `floor(N*0.99)` 项；Translate 样本数较少，
“最慢 1%”实际上只含一个样本。Draw 总计 1749.602 ms，即约 18.225 ms/批；
Preflight 总计 342.485 ms，即约 3.568 ms/批。主要是频繁操作累积，少量慢 Draw
也有贡献。最大 Draw 6.656 ms，不能在没有调度证据时直接归因给某个 Vulkan API。

## 分离驱动编码与准备阶段

全量 detail 产生了一个丢块标记，并把 sampled Draw 明显拉长，该文件只能作热点线索。
新增 `profiler_detail start 32`：每 32 次 Draw 采一整次内部细节，基本 Draw、Translate
仍完整；非 Draw 的细节仍保留。`CPU.Detail.DrawStride` 进入 PROF，状态命令也返回
`cpu_draw_stride`。`NVN.Draw.Sample` 标记选中的总体，不能将部分子区间除以所有 Draw
次数。无参数 `start` 保持每次采样。

首份 1/32 采样，PID 24696，3.005 秒，70 个 Present marker，7944 个完整 sampled Draw：

| 范围 | 平均 | P99（直方图上界） | 最大 |
|---|---:|---:|---:|
| NVN.Draw.Sample | 10.345 μs | 34.815 μs | 435.417 μs |
| NVN.Draw.Encode | 1.315 μs | 6.527 μs | 43.438 μs |

Encode 从 occlusion/Pass 切换到 pipeline、descriptor、vertex/index/dynamic state 绑定和
最终 draw 录制结束。它仍包含适配器的去重、状态校验和计数，**不是纯驱动调用**。
内部细节会增加被抽中 Draw 的耗时，因此不能把 sampled/unsampled 的平均直接相减，
也不能把这个样本的百分比视为无采样运行的精确占比。它已足以否定“18 ms 都花在
vkCmdDraw”的解读。采样无丢块、无 CPU 配对错误；一个 GPU 边界区间不完整，
GPU/CPU 时钟对齐未知。

## 两项准备路径调整与实际收益

- 读依赖处理完成后，恢复图像 front cache。稳定纹理描述先复制，再处理 CPU 依赖，
  然后才查询缓存；保留 epoch、提交序号、上传状态及图像存活检查。
- 动态 UBO 缓存保存 canonical binding/array 顺序及 Guest source slot。每次命中仍
  从当前地址获取快照，只省去重复构建、排序中间 Buffer 列表，保留 Buffer 身份检查。

`renderer_schedule images on/off` 与 `descriptors on/off` 在引擎边界非阻塞切换。
PID 19876 的六窗口 A/B，每窗口 20 秒，测帧率时关闭 Litep，随后单独采 3 秒：

| 配置 | FPS |
|---|---:|
| 两项关闭，A | 22.93 |
| 仅图像缓存 | 23.06 |
| 两项开启，A | 23.11 |
| 仅 UBO plan | 22.12 |
| 两项关闭，B | 22.97 |
| 两项开启，B | 23.25 |

这些变化不足以说明获得了明显提升。基本 trace 中关闭项平均 Translate 25.332 ms，
开启项 26.702 ms，也不支持“已经降低到某个新值”的结论。保留完整数据，不以单个
窗口微小变化包装性能改善。

## UMA 内存策略

同一 Odin3 Turnip 实际提供：`0:0x7/h0 1:0xf/h0 2:0xb/h0 3:0x11/h0`。
此前 stream arena 使用 type 0 (`DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT`)，
type 1 还具备 `HOST_CACHED`，且处于同一 heap。Mac MoltenVK 原本就使用 type 1 / 0xf。
日志 `arena chunk cached=0` 是旧的 arena 用途字段，不能据它判断 Vulkan 内存属性；
应看 `memoryType` 和 `flags`。

新的共享诊断策略 `MNG_UPLOAD_CACHED` / `debug.mng.upload_cached` **显式开启后**，在资源 memoryTypeBits
允许时优先选择同时具备设备本地、CPU 可见、一致、可缓存的类型；随后回退到一致的
CPU 可见类型，最后回退到需要 flush 的 CPU 可见类型。不会仅为 CPU 缓存切到一个
非设备本地 heap，也不省略非一致内存的 flush。分配模式需重启才完整生效，不能在
旧 arena 尚存时把开关变化当作 A/B 成立。

CPU 缓存选择只是第一步。原实现每帧仍有约 20.26 MiB vertex、2.40 MiB index、
14.68 MiB uniform 和 0.11 MiB storage 快照复制。UMA 不等于可以取消所有快照：
提交后 Guest 改写、GPU 写回和别名仍需要明确版本与存活期。稳定资源应进一步区分
持久驻留、脏区更新、临时版本化数据，而非一概逐提交复制。

## 并行化结论与后续边界

Mac `nvn_render_stream.h` 将已解析并持有对象的命令包交给专用编码线程；prepare 和
encode 重叠，GPU 顺序不变，提交/非 render 操作前 join。它并不是随意并行执行
多个带共享状态的 Draw。`nvn_mtl.m` 的 uniform snapshot 还先收集、排序、合并范围，
再交给辅助线程复制；消费方能自助完成尚未开始的条目，GPU 写入会使相关快照失效。

共享 Vulkan 当前不能直接把 draw() 投给多个 JobSystem worker：State、图像状态、
快照 arena、descriptor pool、static scratch vector 和 command pool 都有单一 owner。
合理的拆分顺序是：

1. 先优化内存类型、稳定数据复用和描述符准备，避免并行执行可被消除的工作。
2. 将 host 资源及动态偏移解析成不可变 packet，单独编码 owner 保持 Vulkan command
   pool 的外部同步；记录 post/begin/join/queue-full，验证等待没有被转移。
3. 再对已确定读集合的快照或 pipeline build 使用 Foundation JobSystem。调度单位为
   批次，不能给数千个微秒级 Draw 各建一个 job。必须隔离 GPU 写回、inline update、
   别名及资源回收的依赖边界。

本轮尚未把 Vulkan 编码移到工作线程，不宣称已并行化翻译。稳定场景中没有 pipeline
compile，优先并行编译不能解决这里的持续低帧率。

## 复现与证据

- 自动启动、存档打开和游戏负载门控：`android/tools/validate_scene.py`。
- 同进程路径 A/B：`android/tools/benchmark_translation.py`。
- 需重启的内存类型 A/B：`android/tools/benchmark_memory.py`。
- 脚本以 PID 和 BOTW 1.6.0 main Build ID 为门槛，保留配置、原始 PROF、sidecar、
  SHA、失败信息；自动恢复诊断开关。游戏负载门控不是地图/相机语义识别。
- FPS 为 successful native Vulkan presents，不是 compositor 实际显示时间戳。
- 工作区外证据：`native-switch-study/android-translate-20261026/`。
- 初始完整 PROF SHA：`bece30a2704e0bedc7c3e66f790f622aa2d9bf1669537f064af7a85764bb9def`。
- 1/32 detail PROF SHA：`df831772bd4a49aee75e9e8961f80bb7937a672e2e42c11d8a98499fda7aefff`。
- GPU probe：Odin3 Turnip 395 次提交，Mac MoltenVK 392 次；新增 16384 个动态 UBO
  回读像素，覆盖乱序 metadata、数组、地址改变、CPU flush 与 slot 复用。另有内存
  类型可用性/回退测试、debugbus 与采样模式测试。游戏完整画面还需独立观察。

## 批次线程 CPU 时间与 UMA A/B 复测

新增每个 Translate 两次 `CLOCK_THREAD_CPUTIME_ID`，与单调经过时间共同记录。
只统计完整 Translate 区间内同线程 counter，按区间包含关系配对，不用事件序号猜测。
开启 cached 的第一窗口有 69 个完整批次，平均经过 **26.537 ms**，线程实际执行
**26.193 ms**，差值 **0.345 ms**，CPU/elapsed 为 **98.70%**，所有线程时钟有效。
关闭 cached 的窗口另有 71 个完整批次，经过 **25.438 ms**，线程 CPU **25.182 ms**，
差值 **0.256 ms**，执行占比 **98.99%**，线程时钟同样全部有效。
这证明两个采集窗口的同步翻译本身主要消耗线程 CPU；不表示其他 Guest 线程没有等待，
不包含其他驱动线程的 CPU，也不能用差值区分 sleep 与 runnable。Litep 仍有采集扰动。

同 APK `e569f3591dcea1116d6d70f2079051eacd2bb49dc84d863d219fef24cba96950`，
同原存档自动启动流程，每次暖机后独立测 30 秒（测 FPS 时 profiler 关闭）：

| 顺序 | PID | 实际上传类型 | FPS | 每 5 秒范围 |
|---|---:|---|---:|---:|
| cached A | 6507 | type 1 / 0xf / heap 0 | 22.881 | 22.34–23.43 |
| coherent | 8137 | type 0 / 0x7 / heap 0 | 22.638 | 22.40–23.07 |
| cached B | 10694 | type 1 / 0xf / heap 0 | 22.772 | 22.19–23.15 |

差异约 1%，窗口范围重叠，没有足够证据称为改善。**最终 shared policy 默认保留
coherent-first（upload_cached=0）**，cached 作为有实机证据、可回退的诊断选项。
Mac 和 Android 仍使用同一策略，Mac 原有 coherent 候选本身就是 cached。

前两次测量因 Cemu 崩溃弹窗盖住 BOTW 而失败，失败文件完整保留；未采用被遮挡阶段。
第三轮在加载阶段继续等待前台稳定，采样阶段每 5 秒复查前台，三个窗口均完成。
这些是重新加载同一存档的匹配工作负载，未通过语义检测证明精确相机姿态，温度亦未锁定。

证据为 `android-translate-20261026/uma-ab-3/`；cached A 原始 PROF SHA：
`0cfea771f7c219c0c8311e3e0447b28afab292fa6256823b547f8d71202ad047`。
其 3.018 秒有 70 个 Present marker，无丢块、无 CPU 配对错误；一个 GPU 边界区间
不完整，跨时钟对齐仍未知。完整 SQL 索引保存在 `index-on-a/` 和 `index-off/`。关闭 cached 的 PROF SHA：
`6b62a096b8ad2e8baae13485a99f55d7dc5077184d0645fae551e5534d33b6d7`，3.005 秒、
72 个 Present marker，同样无丢块或 CPU 配对错误，仅一个不完整 GPU 边界区间。

### 跨帧资源复用缺口

已检查 `nvn_mtl.m` 的 `mtl_pool_add` 使用 `newBufferWithBytesNoCopy` 包装对齐 Guest
范围，uniform 另走快照与 GPU 写入失效机制。D2R Vulkan 自身有 `vcache` 持久不可变
顶点/索引副本：通过 `nvn_dirty_now/protect/since/tracked` 追踪脏页，过预算后按 Fence
延迟回收旧 arena。当前 Android 底座没有实现这些导入，不能仅打开 `DM_GFX_VCACHE`
就宣称启用了缓存，也不能在没有脏页跟踪时跨提交复用快照。

下一步资源改造需先接入与现有 Guest 故障处理兼容的脏区来源和 generation，随后区分
不可变、稀疏更新和高频临时资源。GPU 写回、映射别名、pool finalization、队列在途
引用都必须进入失效条件。只有测到该路径实际命中、复制字节减少并通过改写/回收测试，
才可默认启用；直接把共享物理内存当作同步协议不成立。


后续已补齐 NVN 状态／PSO 模型、缓存提前命中 A/B 和整机占用复核，见
[NVN 管线缓存记录](NVN_PIPELINE_MODEL.md)。单个 Translate 的线程 CPU 占比接近
99%，不能写成整颗 CPU 已跑满；实测总 CPU 24.39%、GPU 设备 busy 28.45%。
