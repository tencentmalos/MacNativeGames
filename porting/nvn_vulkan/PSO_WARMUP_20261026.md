# BOTW 的 Foundation PSO 准备队列

## 当前落地范围

Mac / Android 的共享 NVN→Vulkan 后端已使用 Foundation `WORK_JOB` 预热 PSO，
前台按原 Draw 顺序领取结果。每帧的状态翻译、资源准备及 Vulkan 命令发布仍由
原线程执行，尚未成为多个并行 Draw packet。不能把这一步描述成完整消除 25 ms 翻译。

原 D2R 后端已经有后台预热线程，Odin3 上原默认值为 2。原先前台找不到 ready pipeline
就同步新建，即使后台已经在编译相同 key；可选 runtime async 还会跳过部分 Draw。
本次保留其完整序列化 key 和 PSO 构建器，改造调度、去重、输入所有权及结果交接，
禁用跳 Draw 路径。同时修复两个预热线程同时创建相同 PipelineLayout 时，未销毁
竞争失败副本的问题。

## 执行与生命周期

- `porting/jobs/KeyedJobs` 链接真实 `foundation_async_task`，自己不创建线程池。
  构建引入固定 Foundation 的 core / reflection / allocator / property，继续只保留
  一个 profiler ring / SDK 实例。两端使用同一份依赖与队列实现。
- 任务拥有 `Rec` 的 shader key 和序列化状态字，不携带指向栈上 VkCreateInfo 的指针，
  也不读取将被 Guest 改写的 UBO / 纹理地址。worker 只创建 PSO 及其预热依赖对象，
  不操作 command pool、descriptor pool、NVN 全局状态或同一 VkQueue。
- 最多 4096 个任务和 64 MiB 输入/key 预算；同一 key 的重复请求只复用任务。
  超预算或未知 key 正常回退前台构建，不少画一条命令。
- 状态为 queued → running → ready / failed。前台需要 queued 任务时可直接领取，
  running 任务则等待已有结果；ready pipeline 的原有缓存、淘汰及 fence 退休规则保留。
  前台领取可能使执行峰值达到 worker 数加一个。
- 构建完成后释放任务输入，保留有界 key 和结果状态；退出先禁止入队、取消未开始任务、
  等待正在运行的构建，再销毁预热 shader / layout / render pass / pipeline。
- `MNG_PSO_WORKERS=0` 自动取 `min(4,max(1,cores/2))`，允许显式 1..8。Android 仅传输
  `debug.mng.pso_workers`，不另维护平台默认值。

PipelineCache 创建时没有 EXTERNALLY_SYNCHRONIZED 标志；预热线程与前台共享 cache 的
用法符合普通 Vulkan 对这一标志的同步约定，未额外引入全局 compile mutex。
语义依据：[VkPipelineCache 创建说明](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreatePipelineCache.html)。

## 实机证据

设备 Odin3 / Adreno 830，BOTW Switch 1.6.0，main Build ID
`8E9978D50BDD20B4C8395A106C27FFDE`，720p 手持、内部 1x、60 FPS 补丁、共享策略。
Turnip SHA-256 `e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`。
APK SHA-256 `b863cf67f4a7e3109a8a94f95aeb26f6a54334c9d9c0d399d95f82717bfd8a24`，安装后读回相同。

每次保留 driver cache，使用同一 2255 条预热列表，列表 SHA-256
`1e0071336fc9aa616233fbba1b1a086b4d16bd39450569d1cf6172c155cf2a14`。
这是已有 driver cache 下的 PSO 准备吞吐，不是冷 shader 编译测试，也不是旧版本二进制 A/B。

| worker | PID | 完整 PSO 数 | 首个 Build 开始至最后完成 | Build 区间并集 | 实测并发峰值 | 无 profiler 场景 FPS |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 7695 | 2255 | 68.476 ms | 64.654 ms | 1 | 23.549 |
| 4 | 9105 | 2255 | 46.982 ms | 46.863 ms | 4 | 22.563 |
| 2 | 11963 | 2255 | 60.571 ms | 59.843 ms | 2 | 22.570 |
| 4 | 13353 | 2255 | 41.129 ms | 40.953 ms | 4 | 22.484 |

四次任务全部成功；实际场景需要 PSO 时已准备完成，所以此次实机 `joined/claimed=0`。
前台领取与等待路径由合成并发测试覆盖，不能声称实机场景已经测得消除了这类等待。
4 worker 两次存在至少两个 Build 重叠的时间为 46.390 / 40.354 ms。各 lane 累计经过
时间分别为 172.929 / 149.595 ms，不能直接当作启动耗时或 CPU 占用；增加 worker 仍有
竞争及调度成本。表中数值由原生 Litep interval 查询与区间并集计算，未相加嵌套 scope。

帧率在自动读档、暖机、前台稳定后采 20 秒；采样时 profiler 已关闭，使用成功 native
present 的计数和样本年龄修正。场景门控证明存档加载及游戏渲染负载，不证明精确相机
或地图坐标。没有可确认的稳定场景 FPS 增益：2 / 4 worker 都约 22.5 FPS。
启动时曾出现 Cemu 崩溃弹窗；脚本未在失去前台期间取帧率，停止该进程后重新稳定再采，
未清任何应用数据。此轮不作严格温度/频率锁定的硬件加速倍率结论。

### 采集完整性

`profile_startup=1` 启动有界 Litep 文件采集，使用 SDK 的 `apply_mode_transition()`，
不伪造第一帧。四份文件的 PSO Build 完整数均为 2255，payload/container complete，
0 skipped chunks、0 scope pairing errors，无解码告警。捕获末尾未闭合的 Guest 等待
属于停止边界，不计入 PSO 构建。采集在首个实际 present 前结束，故没有帧标记；
只用于启动任务吞吐，不推断 GPU 利用率或游戏帧率。

第一次 ring 测试停得过早，得到 `profiler_ring_awaiting_source_frame`；第二次 ring 文件
存在解码失败且没有覆盖首帧前预热，均保留为失败证据并排除。最终只接受后续 file 采集。

原始文件在 Git 外的 `native-switch-study/android-translate-20261026/`：

| 案例 | PROF 相对路径 | SHA-256 |
|---|---|---|
| 1 worker | `warm-ab-3/0-workers-1/litep/file-1793009081135650-1.prof` | `7767269904db581085d4f49f4a829667c70867441f824a8584ad9693ae1f4f40` |
| 4 worker | `warm-ab-3/1-workers-4/litep/file-1793009142589912-1.prof` | `5d4d5580fb3efbece51293b2823a9907affe8effc04921251b5b4492692298b6` |
| 2 worker | `warm-ab-4/0-workers-2/litep/file-1793009316399732-1.prof` | `a7f999bcac355e6749efca3c3cda4103d8b15cfd679623c41853316796e1664f` |
| 4 worker 重复 | `warm-ab-4/1-workers-4/litep/file-1793009378223777-1.prof` | `e8e0a23eda9fa44c59ab5c57ab6c01eaac49d89a8824f50dbc1c641ffb7b1d72` |

`warm-task-overlap.sql/json` 保留实际查询和结果，`warm-ab-3/4/result.json` 保留 APK/PID、
输入列表前后哈希、实际选项、未插桩帧率与属性恢复状态。

## 稳定场景复核

最后保留 PID 13353、4 workers 的场景进程，独立采 3 秒基础 Litep：70 次完整 Translate，
平均 26.230 ms；255717 次 Draw，累计 1309.761 ms，单次平均 5.122 µs。
没有 `NVN.PSO.Build/Wait` scope，PSO 累计 built 仍为 2255、active 为 0。
同批次的线程 CPU 计时平均 25.957 ms，匹配计时区间经过 26.216 ms，差额约 0.259 ms；
这表示该翻译区间主要在执行，不表示整颗 CPU 已满载。剩余热点仍是正常每帧的同步
翻译，而非持续 PSO 编译。

文件 `warm-steady-final/file-1793009567475340-2.prof`，SHA-256
`27f2159a9994752cab80d78679c4c3f7a0903053ce92ab9ab1011ee357760083`；
CPU payload 完整、0 skipped chunks、0 配对错误，70 个源帧标记；存在一个捕获边界的
incomplete GPU zone，不用它判断 GPU 忙闲或跨 CPU/GPU 的重叠。采集结束后 recording=0、
capture_active=0，启动属性已恢复为空，当前 4 worker 与自动默认值一致。

## 验证与下一步

- Mac / Android 的 KeyedJobs 测试通过真实 Foundation worker 验证并发、同 key 去重、
  queued 前台领取、running 等待、异常结果、预算拒绝、停止时取消队列与等待在途任务。
- Mac MoltenVK 与 Odin3 Turnip 的 GPU probe 均使用生产构建器并发重建四个实际 draw
  生成的 PSO，各产生四次去重；完整像素、缓存淘汰、资源生命周期及 fence 测试通过。
- Android APK assemble/lint、Mac Vulkan/Metal 构建、debugbus/profiler 与共享策略测试通过。
  porting 的 23 个 Python 测试通过。额外全仓测试有五项环境/既有失败：四项缺少 Python
  cryptography，另一个 VisionOS MetalFX probe 编译失败；不将全仓测试记作全绿。

下一阶段要把每帧的重复准备移出同步翻译，不能继续靠增加 PSO worker 解释 25 ms。
先以 program / descriptor metadata / state generation 为键持久化准备计划，再按批次
拥有 UBO、间接参数、纹理代与命令快照，交给 worker 做无副作用准备；解码线程按依赖
顺序领取结果，完成 descriptor 更新、hazard / pass、bind / draw 及真实 fence 发布。
状态改变、资源别名和 GPU→CPU 读回仍必须作为依赖边界。用户要求的“快速聚合并发布
Draw”方向保留，但这份 APK 尚未完成该阶段。
