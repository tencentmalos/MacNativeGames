# BOTW 原生内存与 NVN→Vulkan 接入

当前只推进 BOTW Switch 1.6.0，Android 开发 APK 为 `0.5-native-startup`。
原始 SDK、模块构造函数和 `nnMain` 已接入，APK 已实际提交 Android Surface 帧。
是否达到可游玩场景必须以 [Android 验证记录](../../android/VALIDATION.md) 的实机证据为准；
接口注册数和合成 GPU 测试不能替代游戏验证。

## 实现与参考边界

| 部分 | 实现 | 参考与限制 |
|---|---|---|
| 内存 | `../runtime/guest_memory.*`，39 位地址空间、堆、物理页、栈映射、权限和查询 | 对照 Mac 原生策略及 Citron SVC 语义，自行实现宿主映射 |
| NVN 前端 | `nvn_vulkan.cpp`，当前注册 268 个入口，编译期校验可得原型 | 对照 Citron、MacNativeGames、D2R；数量不是完整覆盖率 |
| Vulkan | 固定 D2R `758f5d41509af5cd38b7eb934bc2334bdb3e3342` 独立引擎 | 不链接 D2R 宿主或游戏补丁；`prepare_engine.py` 生成构建目录内的适配副本，参考检出只读 |
| Shader | MacNativeGames 翻译器离线产生 SPIR-V，再打包 XGS | 身份包含控制段和机器码；Mac metallib 和 D2R 游戏 shader 不能直接用于 Android |
| 平台 | Foundation 输入、音频；Android Surface / Vulkan swapchain | 游戏运行在专用 `:game` 进程，生命周期仍需完善 |

## 对象、录制与同步

NVN 对象按所有权分类：纹理使用经过大小校验的 128 字节内联值，sampler、builder、buffer 和 sync 也按值保存；队列、程序及内存池等仍使用 8 字节宿主句柄。
纹理视图采用可按值复制的 16 字节表示；零计数在绑定时按具体纹理解析为剩余 mip/layer，getter 保留原值。BOTW 会复制纹理对象；重新初始化产生新身份，
销毁一个副本只清除该副本的值，不修改其他副本。录制和描述符使用不可变快照。
GPU 图像由独立 storage 表拥有，serial 仅作查找身份；销毁副本后仍保留有效 GPU 内容，内存池销毁则使重叠 storage 失效，并按提交槽退休。

每个录制器使用有界宿主命令存储，避免扩大的宿主命令覆盖 guest 的共享录制区域。
控制内存同一位置重新录制会复用句柄地址、更新其内容；`CallCommands` / `CopyCommands`
在录制时保留被调用内容的快照。已录制调用不会随随后对源区域的覆盖而改变。
`GetCommandMemoryUsed` 与宿主记录容量分离：顶点属性/流/缓冲、UBO/SSBO、纹理/图像绑定、
索引绘制沿用 Mac 前端已建立的命令计量，复制后保留源计量。其余指令仍使用兼容记录用量，
尚未逐项证明全部 Maxwell 编码；实例化绘制的计量继承 Mac 的假设，不能当作硬件实测。
描述符由后端按值持有，不额外占用录制用纹理快照池。录制器销毁时回收宿主记录，已结束录制的命令 handle 独立存活；程序和内存池的宿主记录仍需分类型观察长期保留量。

`QueueFlush` 只提交待录制工作；`QueueFinish`、资源销毁和部分读写路径仍保守地等待 GPU 完成。同步结果依据真实完成状态。
Present 不再人为 sleep；逐交换纹理的节拍限制在 Acquire 执行，并在 sleep 时释放前端锁。
GPU counter 经同队列校准映射到 Guest 的单调时钟，不能直接暴露设备原始时钟起点。
长宿主调用按 Mac 的契约释放 Guest 虚拟核心，并在释放宿主锁后重新获取核心。默认仍是同步回放；可选有序翻译仅接收通过所有权检查的提交，失败时保持同步。
counter report 的输出代次、GPU 退休和 CPU 可见性约束见 [COUNTER_REPORT_20261027.md](COUNTER_REPORT_20261027.md)；最新 [Odin3 对照](ODIN3_COUNTER_REPORT_20261027.md) 已越过 report 首阻断，但真实整批仍在 Draw 输入检查处回退，尚未证明提速。
当前共享 Vulkan 的 [活动范围 Flush 实测](ODIN3_LIVE_FLUSH_20261027.md) 已减少无关 Guest 线程的全局锁等待；四轮 Odin3 A/B 尚未证明稳定 FPS 提升，剩余图像冲突与同步翻译仍需继续处理。
高频临时对象和纹理缓存已经加入回收与观测；这不代表所有 NVN 对象或游戏场景都已验证。实际架构差异、Litep 结果和长期运行边界见 [ARCHITECTURE_ALIGNMENT.md](../ARCHITECTURE_ALIGNMENT.md)。

原生 BC 图像允许尺寸兼容的整数块视图，framebuffer 尺寸按所选 mip 的块数计算；
每个未压缩视图限一个 mip 和一层。实现依据 [Vulkan 图像视图规则](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageViewCreateInfo.html)，
Mac 的块附件处理提供游戏使用方式的对照。设备和实际格式能力检查仍保留。

线宽参与管线状态及缓存键，并按 Vulkan 设备能力检查。固定点大小当前只接受离线
翻译器已固化的 1.0；shader 自己写入的 PointSize 保留，其他固定点大小明确拒绝。

## 内存和服务

已接入 `SetHeapSize`、`SetMemoryPermission`、`MapMemory` / `UnmapMemory`、
`QueryMemory`、`MapPhysicalMemory` / `UnmapPhysicalMemory`，以及 BOTW 实际使用的
`GetInfo`。原生线程、虚拟核心调度、IPC、只读 RomFS/DLC、账户、存档、系统字体和
音频渲染器共享 Mac 实现，Android 分支维护 Bionic TLS、宿主栈和 Linux 信号语义。

物理 NVN 池以 memfd 保留初始内容，虚拟池映射到相同物理页；映射请求先整批检查再应用。
堆与物理映射共享逻辑预算。统计是 guest 预算，不是 Android RSS。
16 KiB 宿主上以有效子页权限并集保护宿主页，无法单独硬件保护同页内的 4 KiB 子页。

Tiled-cache action 由 Vulkan 驱动管理物理分块；当前六种有效 action 均保守结束 render pass
并执行完整 GPU 内存屏障，tile-size 提示不改变 viewport/scissor。此策略优先保证顺序，尚未优化屏障数量。

未支持的接口和参数保留显式失败。ZCull 不分配额外保存区，压缩和深度剔除元数据归 Vulkan 驱动所有。
BOTW 同步修正由 `games/botw/guest_patches` 的精确身份配方负责，不放进通用 NVN 前端。

## 离线着色器

`android/tools/prepare_botw_shaders.py` 消费已验证的私有 RomFS、模块和 Mac shader 元数据，
调用本仓库的 MacNativeGames 翻译器生成 Vulkan SPIR-V，经过 `spirv-val` 检查后导出 XGS。

- shader 键为 SHA-256（有效控制段＋完整机器码）的前 16 个十六进制字符，离线报告保存完整哈希。
- BOTW 的短控制段按自身长度处理；不能固定读 0x880 字节。
- 相同控制段可能对应不同机器码，仅控制段哈希会混淆不同程序。
- 启用 BOTW 的 `depth_minus_w` 翻译参数；驱动依然需要建立 Vulkan pipeline，离线 SPIR-V 不等于驱动机器码缓存。
- 同一 shader 的元数据变体全部验证通过且 XGS 相同才合并；不同结果通过有界 XGV 索引
  在 draw 时按实际顶点输入/片元输出类型选择，选中模块按内容哈希复用。纹理/图像查询
  特化暂不支持，保留缺失；Metal 专属深度纹理声明不作为 Vulkan 特化条件。
- 未绑定顶点输入使用引擎提供的类型匹配零值；没有匹配项则失败并记录状态，不跳过绘制。
- 资源槽对照 Mac 的 combined handle 表。所有游戏派生文件保留在 Git 之外。
- APK 按 manifest 哈希划分缓存代次，逐文件检查 SHA-256；归档通过单个 SAF 流导入，保留旧代次，不清空用户缓存。

## 验证

`mng_vulkan_probe /private/writable/test-directory` 使用手写测试 shader，
验证真实 GPU 清屏、索引绘制和计算，包含：

1. 2048 个像素读回、128 个裁剪区域像素检查、64 个计算输出字检查。
2. 带负 base vertex 的索引绘制。
3. 控制句柄复用、源命令被覆盖后的不可变 `CopyCommands`。
4. 纹理按值复制、销毁和重新初始化，以及对象/视图保护字节。
5. 物理/虚拟内存双向别名、非法映射批次无部分生效、解除映射。
6. 短控制段保护页、相同控制段不同机器码的已知哈希。
7. GPU fence、读回、连续会话初始化和关闭。
8. 八个独立 3D 切片附件及零计数视图的逐像素读回，ZCull 零存储，空池命令容量。
9. 支持原生 BC 的设备上，以 RGBA16UI 附件写入 BC4 压缩块，再读回 16 个原始整数。
10. 启用 custom-border-color 后，采样两种非标准边框颜色并核对 4096 个像素，覆盖缓存区分。
11. 同一描述符槽连续登记 65,537 个纹理身份，不耗尽命令快照池。
12. 按 Mac 参考核对 364 字节的组合命令用量、复制/调用后 728 字节，以及容量重置。
13. 保留 format NONE 纹理的描述符元数据，不在登记阶段创建 GPU 图像。
14. RGB10A2SN 顶点提取的 4096 个像素读回，覆盖正负端点、分量顺序和两位有符号 alpha。
15. 六种 tiled-cache action 穿插清屏、复制和后续绘制后，逐像素检查内容保留。
16. 支持 depthBounds 的设备上，6144 个像素验证深度范围内通过、范围外剔除和禁用测试。
17. 6144 个像素验证 rasterizer discard 开启后不写颜色、关闭后恢复，以及管线缓存区分。
18. 三层 D16 深度图像的 192 个像素读回，验证全层清除、非零起始层子视图和裁剪保留。
19. 6144 个像素验证非索引实例绘制的 first instance、divisor=1 顶点提取、两个实例和零实例。
20. 八个线程执行 4096 组独立纹理 builder、命令初始化/录制/销毁，检查完成数量、命令用量及宿主录制器数量恢复。
21. GPU 写入后按值复制并销毁纹理副本，再从原纹理读回，验证 serial 失效不丢失 GPU 内容；内存池复用相同地址后必须读到新内容。
22. 销毁源录制器后继续复制已录制命令，验证 handle 的独立生命周期；百万次临时对象与 sync 初始化/销毁不消耗永久宿主记录。

23. 真正执行 TOP/BOTTOM 时间戳报告，验证与 Guest CPU 单调时钟同一时间基准且顺序保持；旧绝对计数换算作为已复现的失败负例。

测试只删除自己创建的临时合成文件，不触碰 ROM、密钥、存档和游戏 shader。
游戏场景、图形正确性、实体手柄和实际音频输出需分别验证。

### BOTW 高度图与线性存储

BOTW 1.6.0 的高度图生产输入可使用 `LINEAR_RENDER_TARGET` 单独标志（0x40），
并带显式 stride。该标志与 LINEAR（0x10）都选择 pitch storage。构建适配统一修改
布局、上传和读写判定，避免错误 deswizzle 产生高度条纹和巨大遮挡立面。
`linear_texture.h` 集中维护该判定；固定 D2R 参考源码保持只读。
这与 MacNativeGames `3c482a7` 及 Citron 的线性 NVN 路径一致，不包含任何 guest 地址补丁。

性能采集通过共享 `../profiler` 模块，使用方法与解释边界见 [PROFILING.md](../PROFILING.md)。

### Android 首屏的显示节拍

Present 不再持有前端锁做帧率 sleep。Acquire 默认使用逐纹理可用时间。Android Choreographer 对齐作为默认关闭的实验：完整帧验证显示它转移了等待，尚无收益。GPU 完成与副本所有权不变。宿主默认使用 FIFO；Mailbox 实验不能消除 Odin3 的 QueuePresent 阻塞，故仅保留为启动前 `debug.mng.present_mode=mailbox` 的诊断选项。完整 A/B 数据在 [首屏性能记录](../TITLE_PERFORMANCE.md)。

Android BOTW 默认把 Present 交给单帧容量的工作线程，取得原有引擎锁后才允许 Guest 调用返回；命令翻译保持同步，Acquire/Finish/Finalize 继续等待已有 GPU 完成链。引擎停机先完成 GPU 工作，再关闭记录器并排空工作线程。该行为由 `SerialHandoff` 的所有权、顺序和停机测试覆盖，实机性能及边界见上述首屏记录。

### 采样视图与 shader 类型

BOTW 的字形图集是普通 2D R8 纹理，但部分文字 shader 使用 2D-array 签名。
共享引擎参考 MacNative 的 `tex_type_view`，按已解析的 shader image flags 创建匹配视图；
2D/array 互转保留 mip、所选首层和 swizzle。采样缓存同时区分完整 image flags，
不能只用 texture handle 和标量类型复用视图。GPU 回归交替两种 shader，验证层索引钳制和 Alpha；
Mac 游戏内文字已验证恢复，Android 尚需实机回归，见 `macos/VALIDATION.md`。

### 发布解耦 W0 诊断

`frontlock_status` 提供按 API / Guest handle / 开始等待时持锁 API 聚合的有界统计，
默认关闭；`renderer_ordered_inputs` 提供只读输入拒绝诊断。共享策略与统计口径见
[RUNTIME_POLICY.md](RUNTIME_POLICY.md)。W0 验收通过前，不启用发布队列或扩大有序翻译覆盖。

### 发布解耦 W1

共享 Vulkan 可通过 `renderer_publication on` 验证有界队列，默认关闭。
Compare 使用与原实现逐字段一致的只读比较，不依赖引擎锁；空对象仍返回不相等。
队列回归覆盖顺序、溢出同步回退、关闭排空和失败保留；GPU 探针缓存函数指针，避免反复
GetProcAddress 掩盖批量发布。详见 [共享策略](RUNTIME_POLICY.md)，以实机 ABAB 决定是否验收。

## 2026-10-27 W1 实机验收结果

发布队列两轮验证后，最终同进程 ABAB 为 21.129 → 27.993 FPS（+32.49%）。
尚未满足 W0 基线的 P 锁等待门槛，唯一 identity 退休对账也未闭合；按任务书两轮上限停止。
`publication_queue` 默认及最终运行状态仍为 0；TextureCompare 去锁同样受此开关控制。
Mac/Android Vulkan 共用实现；构建、lint、26 项 CTest 与两端三模式 probe 通过。
详细报告：`porting/nvn_vulkan/ODIN3_PUBLICATION_W1_20261027.md`（相对仓库根目录）。

## 2026-10-27 Thor 验证状态

验证目标已按用户要求切换到 Thor。基线 GPU probe 在 publication=0 时触发时钟
正确性断言，安装及性能采样未执行；Odin3 的 27.993 FPS 不是 Thor 的结果。
下一项前置工作是验证 GPU query 到 Guest 时钟的映射，再继续共享 Vulkan 的锁优化。
原始身份、失败边界和保全状态见 [Thor 验证报告](THOR_PUBLICATION_20261027.md)。

## 2026-10-28 Odin3 复测

按用户要求切回 Odin3，复用既有 APK，三模式 GPU probe 全部通过。
发布队列 OFF/ON 合并 21.338 → 27.977 FPS（+31.11%），两对均提升。
Litep 显示条件等待和翻译 CPU 时间下降，剩余前端锁热点仍为 Invalidate。
这不是 Thor 时钟修复或 W1 全部验收通过；实验队列最终恢复关闭。
数据和限制见 [Odin3 复测报告](ODIN3_PUBLICATION_RECHECK_20261028.md)。


## 2026-10-28：范围 Invalidate W2

[Odin3 范围 Invalidate W2：28.425 → 29.387 FPS，+3.381%](ODIN3_INVALIDATE_W2_20261028.md)。共享开关默认 0；本次实机进程开启，启动属性已恢复。

## 2026-10-28：消除 O0 采样成本，继续验证翻译瓶颈

[Profiler O2与S1复测](ODIN3_PROFILER_O2_20261028.md)：20个ring/SDK/LZ4对象最终O2，
通道发射约5.4→0.50ms/帧；未录制仍约29.5FPS。两份联合采集显示同步翻译约23.9ms，
其中线程CPU约23.6ms，不把scope elapsed等同调度等待。
[ADPF实验](ODIN3_ADPF_20261028.md)保持默认OFF：四窗合并+1.942%，长窗口因进程退出
中断，未达到验收条件。下一项为S3 Draw细分抽样与预检降本，Mac/Android继续共用Vulkan。

## 2026-10-30：S3 诊断检查点

[Draw 与范围预检诊断](ODIN3_PIPELINE_S3_20261030.md)将同一次抽样扩展到预检，
细分 descriptor、snapshot 和绑定遍历。两端构建及 Mac 三模式 GPU 探针通过；
Odin3 的恢复稳定性检查失败，尚无有效细分采样或 S3 优化收益结论。
