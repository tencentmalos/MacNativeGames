# BOTW：Android 与 MacNative 的架构对齐

本轮目标是先修正生命周期、调度和输出契约，再根据同一设备的 Litep 证据优化。MacNative 是实现参考；不能直接把 Mac 帧率作为 Android 的性能基线。当前仅推进 BOTW 1.6.0，主模块 Build ID 为 `8e9978d50bdd20b4c8395a106c27ffde`。

## 对齐结果与边界

| 环节 | MacNative 实现 | Android 发现的问题与本轮调整 |
| --- | --- | --- |
| Guest 值对象 | 纹理、sampler、builder 在不透明对象内保存紧凑状态；命令保留快照 | 原先为纹理副本和已销毁的命令录制器永久保留宿主记录。改用 128 字节纹理值、按值 sampler/builder 和不可变录制快照，移除高频 SamplerCompare 的全局查表与锁 |
| 图像所有权 | `nvn_mtl.m` 的 serial 表和 storage 表分离；存储延迟清理，内存池释放时失效 | 只删除 serial 就可能丢掉 GPU 写入结果。新增独立图像存储所有者，Finalize 只移除该代身份，GPU 对象随提交槽退休；内存池复用清理重叠图像 |
| 长期缓存 | 无引用存储超过 600 次提交后清理；临时身份单独处理 | 序号查找缓存超过阈值时清理并失效前端/描述符缓存，保留物理图像；没有身份引用且闲置超过 600 次提交的存储退出缓存。不是提高原有对象上限 |
| Guest 虚拟核心 | `NVN_BLOCKING` 在长宿主操作前归还虚拟核心 | Submit、Flush、Finish、SyncWait、Acquire、Present 等加入相同的离开/归还契约；必须先释放宿主锁再等待重新获得 Guest 核心 |
| Flush / Finish | Mac 提交会提交实际 Metal 工作，QueueFlush 无需等队列空闲；Finish 才等待 | Android 原先两者都调用 Finish。Flush 改为提交待录制 Vulkan 工作，保留 Finish 的完成语义 |
| 录制与回放 | 桌面默认仍同步；可选异步路径额外处理控制内存、资源快照和生命周期 | 录制锁和对象注册表锁已独立于翻译锁；Submit 保留不可变命令及纹理引用，允许并发录制。翻译本身仍同步，完整异步提交前还需补齐描述符/UBO 的提交时快照 |
| 呈现节拍 | Present 记录逐交换纹理的可复用时刻，Acquire 等待 | 去掉 Present 中持有前端锁的人为 sleep；默认使用 Mac `nvn_pace.h` 逐图像节拍；Choreographer 对齐实验未改善完整帧，默认关闭；Acquire 等待释放前端锁；Android BOTW 另用单帧容量的呈现线程交接原有引擎锁，保留 GPU 生命周期保护 |
| Guest GPU 时间 | GPU 报告与 Guest 系统 tick 使用一致的时钟起点 | Vulkan 绝对计数不能直接当作 Guest 时间；加入同队列围栏校准、有效位回绕和校准误差记录，保持真实相对耗时 |
| PCM 输出 | Mac 在回调中填满实际请求的帧数 | Android 原先每次只投喂 240 帧并等待消费，而设备 burst 为 288 帧，造成周期性缺口。改用 Foundation 的三块借用队列，按消费票据复用内存，允许回调连续跨块读取 |
| 输出分辨率 | 按游戏模式和逐游戏补丁决定 scene/canvas | 启动选项只提供 720p、1080p，默认 720p，internal scale 1×。720p 使用原始 handheld 路径（保留游戏自身动态分辨率）；1080p 使用 Build ID、完整镜像 SHA 和原指令共同校验的逐游戏补丁 |
| 诊断 | 原生线程、渲染与资源状态可关联 | Foundation debugbus 增加 `renderer_resources` 和 `renderer_targets`，均为有界非阻塞快照；Litep 保留真实 CPU scope、GPU 查询与实际成功呈现计数 |

Android Vulkan 渲染器来自锁定的 D2R 源码。所有适配通过 `prepare_engine.py` 生成构建目录中的副本，引用仓库保持只读。没有修改 Foundation 的音频或 profiler 实现。

## 固定的手持 / 底座约定

启动选项固定为 **720p（手持模式）**、**1080p（底座模式）**，默认前者，内部倍率 1×。这两个选项同时决定游戏系统模式和宿主输出，不能只改变 Surface。

| 选项 | OE OperationMode | OE PerformanceMode | 基础画布 / Surface | BOTW 逐游戏处理 |
| --- | --- | --- | --- | --- |
| 720p（手持模式） | Handheld = 0 | Normal = 0 | 1280×720 | 保留原始手持基础场景尺寸，不使用 Apple 手机版的 960×540 补丁 |
| 1080p（底座模式） | Docked = 1 | Boost = 1 | 1920×1080 | 精确身份校验后将原版底座 1600×900 基础场景升到 1920×1080 |

APK91 起共享不可变启动配置：JNI、OE 查询、HID 样式和 BOTW 补丁选择消费同一份模式；缺省 Android 使用手持，Mac 未指定时保留底座默认。手持配 1080p 或底座配 720p 的矛盾参数会在 Guest 启动前拒绝。模式切换在下次启动生效，不伪造运行中插拔底座。

`system_mode` debugbus 返回配置值与实际 Guest 调用 `GetOperationMode` / `GetPerformanceMode` 的次数；`renderer_targets` 返回真实附件尺寸和呈现 crop。两者必须同时看，不能将 1920×1080 的交换纹理分配误称为手持渲染尺寸。私有镜像验证确认手持分支的画布和基础场景宽度均为 1280，1080p 补丁也不修改该分支。

本轮 720p 实测的有效 crop 为 1280×720，场景多附件目标曾为 1024×576、1152×648。模式与基础尺寸一致后，游戏运行时选择的低于基础尺寸的目标仍单独记录；1× 不会自动改写游戏的动态分辨率策略。

## 闪黑的可复现缺陷

在真实 Vulkan/Turnip 上先把纹理清成已知颜色，再按值复制并销毁副本，随后读取仍有效的原纹理。旧实现返回了过期的 CPU 存储内容，测试报 `Finalizing a copied texture discarded live GPU contents`。这不是截图判断。

修复分两部分：存储拥有 GPU 图像；ReadTexels 在序号缓存未命中时仍查物理存储，不能直接退回 CPU 数据。另测内存池销毁后在相同地址重新分配，要求读到新数据，避免修成另一种残留图像问题。该测试证明一类生命周期错误；不能仅凭测试通过宣布实际游戏的全部闪黑或几何异常消失。

## 首屏性能优先

首屏阶段按用户要求先处理约 20 FPS 的启动界面。APK92→105 分开验证时钟校准、节拍阻塞位置、宿主呈现模式、显示回调及有界呈现交接；只有时钟修复时仍约 20 FPS，将节拍移至 Acquire 后的新窗口达到约 30 FPS。APK106 接入完整 60 FPS Guest Patch 后，标题基础窗口平均 16.655 ms；随后按用户最新要求继续场景内优化。原始证据与验证边界见 [首屏分析](TITLE_PERFORMANCE.md) 和 [优化迭代](OPTIMIZATION.md)。

## 本轮性能证据

APK79 与 APK81 使用相同存档加载流程、相同 docked 配置，在 Odin3 上采集。Litep 基础记录分别覆盖 47、63 个完整源帧；每帧约 3644 次 Draw。平均帧时间从 123.66 ms 到 92.76 ms，Guest 条件等待从 63.25 ms 到 36.70 ms，Draw scope 的独占经过时间从 32.01 ms 到 26.91 ms。Fence 仍约 17–18 ms/帧。

这组数据支持继续调查对象处理和同步；未严格锁定相机、温度及瞬时游戏状态，不能把全部差值归因于单项改动。CPU scope 是经过时间，不是 CPU 忙碌率；GPU 未覆盖区间也不是空闲率。新版的 720p 与旧 docked 分辨率不同，性能数据必须单列。

## 仍需继续对齐

1. 检查持有前端锁的等待、每帧多次 Fence，以及 Guest 工作线程的唤醒/虚拟核心重获，按同一帧区间分析，防止只是把等待移到另一线程。
2. 完成异步提交需要的快照与所有权契约，再接入 Foundation JobSystem；参考 Azahar 的不可变 packet、准备 ticket 与有序消费，具体边界见 [JobSystem 方案](JOB_SYSTEM.md)。当前未启用多线程 Vulkan 翻译。
3. Mac 的实时帧步长、输入重复、UI、物理子步与影片间隔已进入 Android 逐游戏补丁，详见 [REALTIME.md](../games/botw/guest_patches/REALTIME.md)。继续验收实际动画、战斗和影片音画；低于 15 FPS 的稳定性限幅仍可能使游戏时间落后，不与 PCM 采样问题混为一谈。
4. 分别验证 720p 和 1080p 的真实渲染目标，区分 scene、present、半尺寸后处理和深度附件；仅有尺寸不能推断具体 pass 的语义。
5. 实机长跑同时观察帧计数、累计初始化数、当前对象/图像/命令保留量和音频消费。合成百万次压力测试不代替长时间游戏验证。

## 按范围同步的实际 A/B（APK90）

同一 APK、Odin3、Turnip 和 1080p 启动选项，分别采集保守 preflight 与按范围同步路径。下列均来自成功 present 的连续完整 Litep 窗口；两个文件无丢块、无 CPU scope 配对错误，各保留一条边界 GPU zone 不完整诊断。

| 指标 | preflight=1（73 帧） | preflight=0（79 帧） |
| --- | ---: | ---: |
| 平均帧时间 | 81.532 ms | 74.721 ms |
| P50 帧时间 | 80.773 ms | 74.489 ms |
| Draw 次数/帧 | 3642.51 | 3654.54 |
| Draw 独占经过时间 | 26.203 ms | 23.882 ms |
| Fence 次数/帧 | 16.01 | 2.00 |
| Fence 独占经过时间 | 18.383 ms | 0.023 ms |
| Present.WaitIdle | 1.073 ms | 15.279 ms |
| GuestCondition | 29.943 ms | 30.666 ms |

不能把 18.36 ms 的 Fence 减少称作整帧收益：大部分等待转移到了 Present。两项等待合计从 19.46 ms 变为 15.30 ms，完整帧窗口只减少约 6.81 ms；还存在温度、调度和游戏状态差异。

更重要的是，游戏自身动态分辨率没有固定：基线附近的主多附件目标为 1536×864，按范围同步后的观测为 1920×1080。因此这是优化方向证据，不是同工作量的因果测速。两个模式均无本轮观察到的 Vulkan 失败，但不能仅以提交成功证明全画面正确；稳定默认仍保留 preflight=1，诊断后恢复 720p、1×。

原始文件分别位于证据目录的 `candidate90-1080/basic` 和 `candidate90-1080-range/basic`，SHA-256 为 `d242d61abdfb282e9e26a795ec8bbed00157db3a2ffece5021c107a8fb194968`、`75a55586d537715e2a86a98481fca395d237c7f7ccd28b2ef0456c5ae29667d2`。逐帧预算和跨线程裁剪结果一并保留。线程间重叠没有被解释成持锁关系；当前缺少 scheduler/KGSL 以及显式 condition-notify 边，不能从其它线程等待推断生产者身份。

本地原始证据目录：`/Users/bytedance/workspace/native-switch-study/porting-20261024-lifetime`。PROF、sidecar、旧 APK 和负例测试日志均保留，私人游戏模块不进入 Git。

## 实机阶段记录

APK87 在用户确认已进入场景的窗口中，78 个完整 Litep 源帧平均 76.24 ms（约 13.12 FPS），P50 73.68 ms；每帧约 3640 次 Draw，独占经过时间 25.69 ms；GuestCondition 28.31 ms；18 次 Fence 共 15.97 ms。此次为 handheld 720p，不能与先前 docked 记录直接作单变量因果比较。实际主要多附件目标为 1024×576，后续有 1280×720 合成和 1920×1080 呈现纹理，Android Surface 为 1280×720。宿主倍率 1× 不等于关闭游戏自身动态分辨率。

用户在安装 GPU 存储所有权修复后反馈“暂时没有闪黑”；这属于当前实机观察，不代表所有场景已通过。APK87 超过 9000 次成功呈现后仍运行；同时发现宿主记录仍增长，进一步定位并修复 CommandBufferFinalize 遗留的 Box，保留命令 handle 的独立所有权。新增并发录制器回收检查和销毁录制器后继续 CopyCommands 的检查通过。

APK90 后续实机场景观察约 988 秒，累计纹理初始化达到 1,117,232 次，超过旧 1,048,576 条上限的触发量；最后样本为第 15,838 次成功 present，宿主对象 10,862、命令录制器 1、物理图像 774。程序/内存池仍有场景资源加载带来的小幅增长，不能宣称全部对象严格恒定；已消除本轮观察到的每帧录制器记录线性增长。完成后主动更换 APK91 验证模式配置，没有将换包造成的 PID 变化当作崩溃。原始逐次快照和结束原因保留在 `candidate90-720-longrun.ndjson` / `candidate90-longrun-summary.json`。

音频设备实际消费日志变为 48 kHz 连续供给，连续观察中 backlog trim 不再累积；三块队列的合成测试跨 1,104,000 帧、不同回调 burst 验证无重复或缺失 PCM。原先每 5 秒只消费约 200,000 帧，新版约 240,000 帧。

另一处性能候选是 D2R 引擎默认启用的每操作 `read_preflight`：在实际 GPU 依赖之前先完成所有 pending writeback/report，导致约 14 次存储写回加时间戳同步。D2R Android 已有按实际 CPU 读范围处理的路径；本移植的合成 GPU 测试在该路径也通过。`debug.mng.read_preflight=0` 仅作为开发版单独 A/B 入口，默认仍为 1；不能仅凭合成测试通过就宣布 BOTW 所有 CPU/GPU 可见性正确。


启动诊断中补齐了 handheld 状态的左右 Joy-Con 连接/插接属性（参考当前 Citron HID 实现）。此前 90 秒脚本窗口结束时仍只有 UI 级 draw，不能据此断言输入失效；后续用户确认场景已进入，渲染计数随即达到约 3600 draws/14 dispatches。读档脚本改为只发送三次确认并等待场景工作量出现，禁止把加载阶段数据计入游戏性能。
