# BOTW：Metal / MoltenVK 的 Pass 与翻译对齐

后续的提交槽、范围 Invalidate 和呈现转换优化见 [Fence 与全局锁分析](FENCE_LOCK_20261026.md)。

本文承接 [上一轮分析](CONCURRENCY_20261025.md)。证据目录为本机
`/Users/bytedance/workspace/native-switch-study/macos-base-20261025/concurrency-analysis-20261026`。
原始 PROF、Instruments trace、游戏资源、存档均留在仓库外。

## 当前结论

同场景 Metal 约 113 个 render encoder，旧 Vulkan 约 465 个 render pass，差异不能笼统归因于 MoltenVK。Vulkan 把同一附件采样连续段里的每个 draw 都当成新依赖，是已经修正的一项原因。反馈连续段先降至约 157 个，进一步按依赖安排 Barrier 和附件后，当前约 130–131 个。Metal 的分层合并仍未移植。

NVN 没有逐个对应 Vulkan `BeginRenderPass/EndRenderPass` 的显式 Guest Pass 边界；这里的对齐对象是实际附件、子资源、读写和同步关系。Metal 的约 113 次是同场景实现基准，不能为凑这个数删除必要的 Vulkan 依赖。多出的复制、边界必须能够解释，并继续检查是否有过度同步。

## 对照身份与测量范围

- Switch BOTW 1.6.0，main Build ID `8E9978D50BDD20B4C8395A106C27FFDE`。
- 复苏神庙静止视角、同一存档；handheld、internal 1×、vsync off、实时补丁上限 120、GPU keepalive off、MFX/TAA off。
- MoltenVK SHA-256 `8ea25172b62b40919bcb60d24c7d93970ac625921a38eb625e66d57f52e8ea8e`，UUID `12331E3A-07E0-3BE1-9BF9-EADB39C2733E`。使用 `lsof` 核验实际映射，未改变 Citron 发布驱动。
- Foundation pin `2f27d3bb41c1659d33b89905255fab2675fc0144`，Litep SDK `e00321ea40c4c608aff74a787200bc59493eea19`。
- 可执行文件按 SHA 冻结在 `macos-base-20261025/binaries/`；每轮保存 PID、Mach-O UUID、参数、环境和存档 SHA。构建不覆盖运行中的二进制，A/B 串行运行。
- 下文节拍均是 `successful_native_present` 的完成次数。没有独立的唯一 Guest frame 身份，不能据此证明每次呈现均为不同游戏图像。用户确认优化版本画面正常，并观察到 50 多 FPS。

## 同步与并发的直接证据

Metal capture PID 50383：`NVN.Translate` 平均 5.305 ms，Replay 4.922 ms，UniformSnapshot 0.178 ms，Commit 0.160 ms。编码 worker 的 Join 平均约 0.032 ms/提交；60,489 对 Post/BeginPass flow 全部匹配，排队延迟 p50 0.459 µs、p99 18.172 µs，无 QueueFullWait。

旧 Vulkan capture PID 54327：`NVN.Translate` 平均 10.796 ms。最重 Guest lane 的 FrontLock.Wait 为 2115.171 ms，其中与**实际记录的持锁区间**重叠 2110.846 ms（99.80%），与 Translate 重叠 2103.740 ms（99.46%）。不是仅凭两个函数同时出现就推断锁所有者。

`metal-timeline.html` 和 `vulkan-timeline.html` 是同一 trace 时钟内的实际区间图。Metal 选定 5.137 ms 翻译窗口与 Encode.Burst 重叠 2.172 ms；Vulkan 图直接展示同步翻译时 Guest 等待同一前端锁。两个独立捕获的绝对时间不拼接。

Instruments 使用本机 avp_study 工具，各录 8 秒，分析 1.5–7.0 秒窗口。Metal / Vulkan 分别有 12,694 / 8,347 个 CPU 样本，缺栈 2 / 777；Vulkan 的缺栈率约 9.3%，保留为 unknown。Vulkan 的 `xbg_submit_now`、MoltenVK worker encode、descriptors、inline_commands 和 cpu_access 都有显著采样权重。采样权重与 Litep elapsed 不相加，也不冒充精确 CPU 利用率。

## 附件反馈连续段

参考 Metal `att_unstored_read` / `g_sd_plain`：首次读取仍在 tile 中的附件前先存储；没有显式依赖的连续采样 draw 共用一个采样起点。关闭 Metal framebuffer fetch 的控制试验仍约 113 Pass，因此不能把差距归因于缺少该扩展。

Vulkan 的实现使用独立 GPU Image 副本，避免在普通 GENERAL layout 下同时采样和写同一个附件。支持单采样、未解码的 2D 颜色附件（含 mip/layer）；不支持的情况保留旧保守路径。

- 同一采样连续段共享副本，描述符绑定副本，颜色输出写原图。
- 显式 Guest 同步、clear、普通写入 draw、目标切换和新的 Guest submit 使连续段失效。
- 副本由原 Image 持有，沿用真实 GPU fence 的退休规则；重复利用通过队列上的复制和 barrier 排序，纳入内存统计，不按每个 draw 永久保留新对象。
- 描述符 memo 和 image-view cache 区分原图与副本身份。
- sampled-slot reflection、目标身份和 descriptor-pool 代数精确匹配后才复用匹配结果。

macOS BOTW 启动器默认设置 `MNG_FEEDBACK_SNAPSHOT=1` 和 `MNG_DEFER_FRAGMENT_BARRIER=1`，显式环境值可覆盖以做 A/B。共享引擎仍需要宿主选择；Android 尚未启用这两个新默认值。不能据 Mac 探针和一个场景的通过声称 Android / 所有标题已经验证。

| 无录制节拍 | 完成呈现/秒 | 限制 |
| --- | ---: | --- |
| 初始 Vulkan | 42.88 | 修改前单窗口 |
| 同一反馈二进制，反馈关闭 | 44.70 | 单窗口 |
| 反馈开启，未缓存匹配 | 49.76 | 单窗口 |
| 反馈开启，缓存匹配 | 52.05 / 52.95 / 52.45 | 三个连续窗口 |
| 只读 storage 分析后的 Barrier 延后 | 54.86 | 单窗口；Pass 仍约 157 |
| draw 元数据和动态状态缓存 | 55.89 | 单窗口；需进一步复测 |

反馈关闭时另一次 Litep capture 出现显著长帧，与无录制节拍不一致；保留原件，不把它与开启样本硬算成翻译加速比。

## Barrier 延后与真实拆分原因

`MNG_DEFER_FRAGMENT_BARRIER=1` 仅在当前 Pass 的 shader 写入限于 framebuffer 时，将已知排序/失效依赖延到真正需要可见性的位置。零值 tiled-cache 操作、未知位不延后。不能把保留依赖解释成删除依赖。

当前只读 storage buffer 依据既有 SPIR-V 写分析识别；无法解析写指针根时按可能写入处理。storage image / storage texel buffer 保守退出。附件读取、目标变化、其他命令、外部写入都会先结束 Pass 并落实 pending barrier。

仅延后原来的 18 位组合没有降低总数，很多边界只是从 Barrier 移到了下一次 draw。新增 `NVN.Pass.EndReason` 将实际原因与触发它的 NVN opcode 分开；不能再把 `op26` 直接当成一种资源依赖。

PID 91483 的稳态约 157 次边界中，目标组合变化约 55、附件读取约 43、非绘制命令约 47、反馈复制约 8、批次提交约 3。随后从 Litep 同线程 sequence 配对得到：未延后的 OpenPass Barrier 主要为 255，其次 17、32、0、1。将已知非零组合纳入同一资源依赖规则后，相关 command 边界降至约 11–12 次/帧；遇到附件读取仍兑现 full barrier。仍不能声称已达到 110 多次。

## 最终控制试验：110 多次已经包含分层合并

Metal `TOTK_LAYER_MERGE` 默认开启。之前的约 112.4–112.8 次/帧中，约 6.2 次为分层 Pass，每帧合并约 15 次 SetRenderTargets。

PID 3872 仅增加 `TOTK_LAYER_MERGE=0`，可执行文件与 PID 50383 完全相同：SHA-256 `d70914549bed9c96f4b072580cf329002da6ff83a5ee6af875c3395477c4d46a`。其余启动设置和存档 SHA 一致。稳态变为 **127.3–128.3 次/帧**，分层 Pass 与合并次数均为零。日志、身份和无录制节拍位于 `metal-layer-off*`；单窗口 78.49 次完成呈现/秒。

Vulkan 最终相同存档运行约 **130–131 次/帧**，而非原先的 465 次。剩余差额已有主要实现来源：尚未实现 Metal 的分层合并。Vulkan 另有约 3 次批次提交边界；这是剩余 2–4 次差额的候选解释，尚未逐个 Guest 事件配对，不能直接视为全部已证明。

附件计划进一步改为按实际采样依赖：被采样且不写入的颜色附件、被采样且所有深度/模板测试关闭的深度附件才移除；未采样的暂时停写附件继续保留，避免 mask/test 状态变化本身产生新目标组合。边界仍由像素回读和明确 Pass 计数回归验证。

分层合并后续需要移植 Metal 的扫描及证明条件，并把每次 draw 的目标 layer 写入实际 shader 输出，保持 mip/layer/view、clear、读取和外部写入的边界。不能只扩大 framebuffer.layers 或丢掉 SetRenderTargets。需要每层不同颜色的正控制，以及故意全部写 layer 0、禁止合并的负控制；本轮未伪装成已实现。

## 翻译侧调整

- CopyCommands 先保留不可变来源及其对象，再一次分配并直接复制 stream，移除中间 body vector 和重复扫描；保留逐条 fallback。
- CPU 读范围的负缓存仅复用已经证明**没有**待写回重叠的精确范围。新 writeback/report 入队必定推进 generation；不缓存未知结果。
- 每个 shader 的 uniform binding 数量只求一次。
- 顶点布局、混合附件计划只在完整相关状态不同后重算；缓存只保存宿主元数据，Guest buffer 仍逐 draw 走快照与依赖检查。capture 时保留逐条属性诊断，shutdown 清理缓存。
- BindCache 扩展到 depth bias、line width、blend constants；命令缓冲切换、clear pipeline 和既有失效点仍清空缓存。改变动态值不创建新 pipeline。

正常粒度 Litep：缓存反馈版本 PID 80560 的 Translate 平均 11.644 ms；draw-plan 版本 PID 91483 为 10.994 ms（310 次完整调用）。顺序样本说明改善方向，不能称为严格隔离的单项收益；最终附件/Barrier 版本 PID 3117 的 300 次完整 Translate 平均 **10.733 ms**，其中同线程 Fence 约 **2.050 ms/提交**，Preflight 约 **1.304 ms/提交**，这些都是嵌套项，不能再次相加。仍明显高于 Metal 的 5.305 ms，翻译两倍的问题尚未解决。

细粒度 capture 把 Snapshot、Descriptor、CPU read preflight、Pipeline 和翻译内的 GPU fence 等待分开。该模式有额外事件开销，仅用于定位，不能拿它的帧率代表正常运行。

## 回归与其他基础设施

最终 Vulkan probe **382 submissions** 在 Metal validation 开启时通过，覆盖反馈连续段、Barrier 后刷新、clear 后刷新、普通 draw 写入后刷新；反馈部分回读 **14,336 像素**；新增只采样深度及重新启用深度测试的 **4,096 像素**，以及 mask 开关连续绘制的 2,048 像素。16 个只采样/写掩码为零的 draw 维持一个 Pass，未采样目标的 mask 关闭/恢复也保持同一个 Pass。既有动态混合 16,384 像素、实例化顶点读取、GPU buffer 写回、纹理生命周期、百万次对象复用、并发录制、不可变 CopyCommands 等继续通过。depth-bounds 当前设备不支持，覆盖仍为 0。

StatusLayer 改为先本地化再测量、同帧先测量再布局；密度按窗口宽高约束。Vulkan 接 Foundation SDF atlas 和 fwidth shader，日志已经确认真实 texture/pipeline 绑定；不把请求 SDF 当成启用成功。尚未单独完成 Android 与各横向窗口尺寸的视觉验收。

Litep desktop controller 补充 shared ring 回复缺少的 PID：命令前后都验证同一个 debugbus endpoint，再补入已验证的 process_id。capture ID、PID 和 SDK revision 继续保留。

正常 Mac Vulkan 启动默认 `DM_GFX_PERF_SAMPLE=0`，与 Android 现有政策一致，避免每 32 帧启动旧细粒度计时。Litep 的有界采集不受影响，显式环境值仍可恢复旧计时。桌面 profiler client 的实际 ring start/stop/status 已验证；共享 profiler Python 测试 8/8 通过。

本轮没有更新 Android 安装包，也没有把 Mac 验证当作 Android 实机验证。最终保留健康游戏进程，关闭细粒度采样和 ring 录制。


## 证据文件

- `litep-metal/mng-metal-concurrency-50383-file-1792982433604856-1.prof`：SHA `2c3ae2fc0b079791be79b588dc50fdcd0490e98901856130239f3228247e2ad5`。
- `litep-vulkan/mng-vk-concurrency-54327-file-1792982751253668-1.prof`：SHA `3e9f3a13fccf0e3a69801b4ba97f0325138caf5ff7893e2083ccdb3885bd5396`。
- `litep-attachment-dependency/mng-attachment-dependency-3117-file-1792988193072627-1.prof`：SHA `e4b740cdebbd704a3ede6e7a638b1424e98ef98bf15446cd919ae429b49982ed`。容器及 payload 完整，丢块和 scope pairing error 为零；capture 边缘有未闭合 scope 和一个不完整 GPU zone，CPU/GPU 时钟对齐仍为 unknown。
- `draw-plan-*`、`attachment-stage.json`、`barrier-kind-distribution.json` 保存通过 Litep 公开查询 API 导出的结果；没有自行解码 PROF 或直接读取其私有 SQLite。
- 最终游戏身份与日志：`vk-final-identity.json`、`vk-final.log`，默认细粒度计时关闭。未采集独立唯一 Guest frame token，也未用图片轮询代替性能采样。

同步规则参考 Vulkan 官方 [Render Pass](https://docs.vulkan.org/spec/latest/chapters/renderpass.html) 和 [attachment feedback loop 设计说明](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_attachment_feedback_loop_layout.html)。普通 GENERAL layout 不提供任意附件反馈保证，不能通过简单删除读写依赖来凑 Pass 数。

最终 PID 4206 无录制完成呈现节拍三轮为 52.84 / 51.07 / 52.66 次/秒（`final-cadence-0/1/2.json`）。这仍是 50 多次呈现，不把 Pass 数降低描述成已达到 Metal 帧率。
