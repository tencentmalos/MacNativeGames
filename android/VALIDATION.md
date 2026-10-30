# Android 运行时阶段记录

## 2026-10-25：先处理首屏节拍

本轮按用户要求留在初始游戏界面，无读档输入。首屏原先约 20 FPS，Litep 证明每帧主要时间消耗在 Present 人为 sleep 和 Guest 条件等待。修正 GPU 时间基准后仍为 20 FPS；再按 Mac 的逐纹理复用语义将节拍阻塞移至 Acquire，APK95 的 179 个完整帧为平均 33.337 ms、P90 33.623 ms，interval 全部为 2。没有加入 60 FPS 或 VFR 补丁。

长窗口补验：Mailbox 和 Choreographer 显示节拍实验没有消除长帧，均默认关闭。APK104 的有界呈现线程在 60 秒内平均 33.899 ms（29.50 FPS）、P90 36.738 ms，同 APK 关闭后为 34.793 ms；APK105 默认开启该交接，FIFO、720p 手持、1× 及保守 GPU 完成检查保持不变。仍有短暂 interval=3 和实际显示长帧，尚非严格稳定 30 FPS，也没有读档验证具体场景。

详细分步对照、负例测试及原始证据见 [首屏分析](../porting/TITLE_PERFORMANCE.md)。此前的场景内结果保留为历史证据。

## 当前轮：模式约定、存储生命周期与 Litep

用户已确认 APK87 进入游戏场景，GPU 存储所有权修复后反馈“暂时没有闪黑”；不能据此宣称所有场景的渲染问题全部解决。新版修复了纹理副本销毁时丢弃 GPU 内容的问题，真实 Turnip 负例先失败、修复后像素读回通过；内存池在相同地址复用也验证过新内容可见。随后补回 CommandBufferFinalize 遗留的宿主记录，已录制 handle 保持独立生命周期。

模式约定固定为 **720p（手持模式）** / **1080p（底座模式）**，默认手持，内部倍率 1×。APK91 将 JNI、OE 返回、HID 及补丁选择统一到不可变启动配置，矛盾参数拒绝启动，新增 `system_mode` 只读 debugbus。两档策略和实际目标尺寸的解释见 [架构对齐](../porting/ARCHITECTURE_ALIGNMENT.md)。

APK90 在 Odin3、同代无 libc++ 依赖 Turnip 上的 720p 基础 PROF 含 79 个完整成功 present 窗口：平均 75.558 ms（约 13.24 FPS），P50 73.074 ms。Draw 独占经过时间 25.241 ms，GuestCondition 28.103 ms，18 次 Fence 合计 16.012 ms；这些不是 CPU 忙碌时间。采集无丢块、无 CPU 配对错误，保留一条 GPU 边界区间不完整诊断。详细桩点另采，不能混入基础性能结论。

1080p 的按范围同步实验减少了 Fence，但多数等待移动到了 Present；游戏动态分辨率也改变，因此尚未修改稳定同步默认。默认仍使用保守 preflight，实验记录保留在 `porting-20261024-lifetime/candidate90-1080*`。

通过的检查包括：真实 GPU 像素/计算/复制/生命周期测试、超过 1,048,576 次临时对象循环、4096 个并发录制器回收、1,104,000 帧变长回调 PCM 连续性、精确 BOTW 镜像补丁/手持分支不变、模式冲突拒绝和 debugbus/HLE 汇编 ABI 检查。长期实机观察与合成压力测试单独记录，不以合成测试替代可游玩或整场景图形验收。

## 2026-10-24：整帧回放与纹理视图修复

APK47 已抓到相邻真实 Present 边界之间的一帧：4660 个动作、3638 次 draw、14 次 dispatch，
末尾存在 `vkQueuePresentKHR`（EID 14253），不再将单次提交误当整帧。
同机回放的最终图像与实机模糊输出一致；EID 7740 的中间 G-buffer 能识别林克和神庙，
但有大块矩形缺损。中间目标随后会被复用，不能仅因它后来变黑就认定该次写入出错。
本次没有完整 NVN Guest 标记链，尚不能把某次宿主 draw 指认为某个 Guest 函数。

发现并修复了独立的视图语义错误：共享 GPU 图像的不同纹理视图沿用原图像的通道映射，
视图缓存键也未包含映射。现在使用注册视图的纹理身份，并将四个分量纳入缓存键；
RGBX 的隐含 alpha=1 先于用户映射组合。附件和 storage image 保持 Vulkan 要求的恒等映射。
`vulkan-test28.log` 在 Odin3 实际复现错误，修复后的 `vulkan-test29.log` 通过四次切换、
8192 像素读回及全部既有 GPU 检查。APK48 正在做真实存档复测，不能据此宣称场景已正确。


## 2026-10-24：临时纹理构造器与整帧诊断

APK43/46 已从原有验证存档进入复苏神庙的运行循环，出现 HUD/操作提示并产生非零音频，
但场景仍为异常放大的模糊色块，未见正常林克和环境，**尚未通过可游玩场景验收**。
因此不能以 Present 次数、持续 draw 或 HUD 作为移植完成证据。

持续运行曾达到 `Native object retention limit`：精确 APK43 ELF 将宿主栈归到
`nvnTextureBuilderSetDevice` 分配临时构造器。构造器没有 Finalize；游戏重复清零临时存储后，
旧实现仍永久保留每一份状态。APK45/46 改为在已验证的 128 字节构造器存储中保存非拥有状态，
复制保持独立，纹理初始化再取得自身身份。`vulkan-test27.log` 在 Odin3 通过 1,048,580 次
清零、初始化、按值复制和尾部 canary 检查；并保留 4096 次并发命令/构造器检查及原有 GPU 读回。
这不代表其他对象的长期生命周期问题都已解决。

APK44/46 的 RenderDoc 抓取只覆盖单次 Vulkan 提交；事件末尾为合成 `End of Capture`，
没有完整场景到 WSI Present 的链，故不作为整帧正确性证据。APK47 加入调试构建限定、
默认关闭的 Pico RenderDoc 队列标记：启动前设置 `debug.mng.capture_enable=1`，
场景稳定后递增 `debug.mng.capture_frame`，在相邻实际呈现边界开始和结束一次应用控制抓取。
验证结束恢复这些属性和原有 GPU layer 设置。普通运行没有标记查询或抓帧开销。

私有证据保留在 `native-switch-study/porting-20261024-gameplay`，不提交 RDC、shader、
游戏模块、存档或密钥。此阶段没有可比较的实际游戏性能结论。


## 2026-10-24：真实 APK 启动与菜单（正在验证）

本阶段基于 `a02228e` 之后的 Android 修改，仅推进 BOTW Switch 1.6.0。
原始模块 Build ID 和清单固定身份保持不变。Odin3 已显示原始游戏的 DLC 提示、标题菜单、
存档选择及加载确认；**尚未确认进入游戏场景，不能把菜单或首次 Present 作为游戏成功**。

已接入原始 SDK 初始化、模块构造函数、Horizon 线程/IPC、RomFS/DLC、独立存档副本、
NVN 调用桥、Vulkan Surface，以及 Foundation 输入与音频输出。输入已实际操作菜单。
当前已补齐命令容量计量、复制后的纹理生命周期、带基址索引绘制和 3D 纹理切片附件。
8 个切片使用不同颜色清除，再逐像素读回，Odin3 验证通过；基础 GPU 测试总计 5 次提交、
1 次 draw、1 次 dispatch、3 个手写离线 shader，不能替代真实游戏正确性验证。

Shader 离线包改用 MacNativeGames 翻译器和控制段＋机器码的完整身份。
19,727 个输入包含 20,013 个元数据变体，当前产出 19,618 个验证通过且变体一致的 XGS。
APK28 新增 XGV 离线索引，58 组不同结果按绘制时的顶点属性或片元输出类型选择，
不在手机上调用翻译器；51 个翻译失败的输入仍显式保留为缺失。
运行时按清单校验每个文件，使用清单哈希区分缓存代次；SAF 单 ZIP 导入避免逐文件打开开销。
Vulkan 驱动管线创建仍存在，尚未完成同场景性能测量。

实机测试中 `connectedBotwDebugAndroidTest` 的 AGP/UTP 清理自动卸载了开发包。
原始共享资产、Mac 原始存档及其他应用未受影响；已从验证副本恢复模块、存档、shader 缓存
和 SAF 授权。后续只直接运行 instrumentation，禁止对持有游戏验证数据的包使用该清理路径。
本轮 instrumentation 曾完成 4/4 且无跳过，但此结果不覆盖后续所有图形补充。

私有证据在 `native-switch-study/porting-20261024-gameplay`：
`apk-run13.log`、`apk19-running.png`、`apk19-input.png`、`apk19-continue.png`、
`apk19-load.png`、`vulkan-test10.log`。后续 APK21 已修复呈现方向及 ZCULL 对齐查询。

APK22 接入精确身份绑定的 `botw160.exclusive-acquire`：正文 18,249 处、重写区 63 处，
应用后摘要 `60474b0e3056daf4aee06c875de30bb001310d934437da0c4e99bdb76ca05007`。
Mac 宿主 3/3 测试及私有 Android 产物的错误宿主拒绝、输出摘要、重复应用拒绝检查通过。
读档已越过之前两次 guest 空指针崩溃的位置，但尚不能认定所有并发故障均已解决。

APK23/24 补齐宿主无需独立存储的 ZCull 查询/保存/恢复，以及零计数视图表示剩余 mip/layer
的语义。Odin3 `vulkan-test12.log` 通过八切片实际读回；341 项仓库测试通过（54.332 s）。
APK25 已将后续失败定位到纹理初始上传请求 464,170,516 字节，超过单个 staging buffer 保护界限；
APK27 已确认该纹理为 83 层 BC 图像。Adreno 830 支持原生 BC，按设备特性启用并由
引擎逐格式核验后，上传保持压缩数据，不再产生 464 MB 展开。其他设备的巨大展开上传
仍需分块支持。`apk-run29.log` 和 `apk25-stack-symbolized.txt` 保留原失败现场。
原生调试 MCP 两次启动因 LLDB 主版本不匹配在附加前失败，没有留下调试会话；此处使用
失败回调的有界宿主栈和精确本次 ELF 符号归因，不宣称完成了 guest 栈重建。


APK28–32 已补齐绘制时的离线变体选择、原生 BC 整数块附件、线宽状态、默认点大小，
以及自定义 sampler border color 的实际设备启用和完整缓存键。离线缓存代次为
`3c1dedf8499a12365aec8e0b1eacb3a70240eb185c8b405957b9554c1c3ef4c3`，
含 19,790 个校验文件，51 个输入仍无可用产物。`vulkan-test15.log` 实测 8 次提交、
4 个手写 shader、3 次 draw、1 次 dispatch，另核对 16 个 BC 原始整数和
两种边框颜色的 4096 个像素；原有切片、存储缓冲、复制生命周期检查仍通过。
宿主变体索引边界/匹配/拒绝测试通过，当前宿主测试共 4/4。`lint02.log` 通过。


APK32/33 再次在 main+`0x1c6d2f4` 空指针写入处崩溃，因此 acquire 补丁并未解决此故障。
MCP 会话 `580476b204074c5fbf51e28bf7bd5462` 在 DAP attach 失败后已清理（TracerPid=0）；
回退同一 NDK 21 的 LLDB/lldb-server，通过应用 run-as 和 abstract socket 抓到分配返回点。
`lldb-game33b.log` 记录 main+`0x1c6d2e4` 返回零、12 层 guest 栈和模型资源堆：
资源 `Model/FldObj_MountainSnow_A_Far-02.bfres` 的线性分配区剩余 64 字节，申请 120 字节。
已导出有界堆/栈现场并移除断点、detach、清理端口。该事实证明局部资源堆耗尽，不能推断全局内存不足。

APK33 去掉描述符登记的多余命令快照；APK34 把主要绑定/索引绘制的 guest 用量与固定容量的
宿主记录分开，参考 Mac 已有计量，并在复制时保留用量。原实现单纹理绑定携带整个宿主数组，
导致游戏资源预算受到宿主表示影响；是否为本次耗尽的完整原因仍须以读档结果验证。
`vulkan-test17.log` 通过 GPU 读回、65,537 次描述符身份轮换、364/728 字节用量及重置检查。
APK34 SHA-256：`76e52e23194269905a0c7cabc005778cf84143ab26362dbd3aeceec042c8252b`。

APK34 越过上述局部堆失败，随后因登记 `format NONE` 纹理被前端拒绝而退出。
APK35 对照 Mac 前端保留这类尚未指定格式的描述符元数据，实际 GPU 命令仍拒绝无格式图像，
没有替换为虚假格式或跳过绘制。`vulkan-test18.log` 通过元数据初始化/登记及全部既有 GPU 检查。
APK35 SHA-256：`0257ee49851abe836af5e6e5ddafad61655baa8a59022514f9dc4a081543c9de`。

APK35 读档继续推进，随后在顶点属性格式 `RGB10A2SN`（0x69）处显式失败。
APK36 对照 Mac 的 signed 10:10:10:2 顶点映射和 Vulkan 格式定义补齐映射，保留驱动能力检查。
`vulkan-test19.log` 通过 4096 个真实 GPU 像素读回，核对符号、归一化、分量顺序和两位 alpha；
总计 10 次提交、6 个手写 shader、5 次 draw 和 1 次 dispatch。没有运行时翻译游戏 shader。
APK36 SHA-256：`4732a623d5fb66dab62f15152aeb85677e48a498353c47ed2d9fd0209a2bb7a5`。

APK36 已越过 RGB10A2SN 错误，随后进入 `nvnCommandBufferSetTiledCacheAction` 缺失调用。
APK37 补齐 action 和 tile-size：物理 tile 策略交给 Vulkan，action 保守结束 pass 并执行完整内存屏障。
`vulkan-test20.log` 通过包含全部六种 action 的清屏/复制/绘制读回检查及既有 GPU 检查。
APK37 SHA-256：`fb7eeb43d8ed0a8a42d1a794ab73f4f72c7345250277ab95aca489b7dd3ef1d2`。

APK37 在加载后段进入 `nvnCommandBufferSetDepthBounds` 缺失调用。
APK38 将入口接入引擎现有的 Vulkan depth-bounds 管线状态，检查实际启用的设备特性和范围。
`vulkan-test21.log` 在 Adreno 830 验证 6144 个像素：范围内通过、范围外剔除、关闭后恢复；
完整检查共 13 次提交、6 个手写 shader、8 次 draw、1 次 dispatch。
APK38 SHA-256：`de88f38a5cbbbde52ac8c82039a7de386a94dbd110e20b8058b7cceb2f67fc95`。

APK38 读档期间的退出来自投屏虚拟键盘断开引发 Activity 重建，未产生新的 native 崩溃。
APK39 声明处理 keyboard/keyboardHidden/navigation 配置变化，并记录原地更新。
Odin3 真实 UHID 接入/断开后 `:game` PID 保持 12555，键盘类型 2→1，菜单继续可用；
证据为 `apk39-hotplug-pid.txt`、`apk39-hotplug-logcat.txt`，`lint03.log` 通过。
此修复只覆盖输入设备配置变化，真正 Surface 销毁后的协作退出仍待完善。
APK39 SHA-256：`daff2efb8c8f191b4b43951e78c2e72a3c3a654075f63811d49143c353549775`。

APK39 已越过深度边界调用，随后遇到 `nvnCommandBufferSetRasterizerDiscard` 缺失。
APK40 将此状态接入 Vulkan 管线与缓存键，丢弃光栅化时禁止首绘制覆盖优化，保留顶点阶段执行。
`vulkan-test22.log` 在 Odin3 读回 6144 个像素，验证关闭→开启→关闭的颜色保留/恢复；
完整检查共 16 次提交、6 个手写 shader、11 次 draw、1 次 dispatch。
APK40 SHA-256：`fecaa1ef5642f061430620b736907e8ece6ccfcb1106b0c385ae60671115bfc9`。

APK40 场景加载停在三层 D16 深度附件清除。APK41 将数组附件清除拆为选中范围内的逐层操作，
复用单层裁剪和写掩码处理；不把一般多层绘制伪装为已经支持。
`vulkan-test23.log` 验证三层 8×8 深度图像全部清除为 0.25，再只把后两层的 4×4 子矩形清除为 0.75；
192 个 D16 值全部匹配，其他层和裁剪外像素保留。完整检查 17 次提交、11 次 draw、1 次 dispatch。
APK41 SHA-256：`b8bfae1cbb620ec795d8721c3ca4191c612ec52d2165a26ace8d6c329e0f22b2`。

APK41 通过三层深度清除，随后遇到 `nvnCommandBufferDrawArraysInstanced` 缺失。
APK42 接入后端已有 draw-plan 路径，保留 first vertex/base instance/instance count，零实例不绘制。
`vulkan-test24.log` 用实例属性 divisor=1 和不同颜色逐像素验证 base instance=1、单/双/零实例，
6144 个像素匹配；完整检查 20 次提交、6 个手写 shader、13 次 draw、1 次 dispatch。
APK42 SHA-256：`dbfdcde7eeb0d2a12ec728e52070de898782b5ff32e880b6f5b60a668e20378b`。

APK42 已执行大量场景 draw，随后宿主在 `std::__tree_left_rotate` 中访问空指针。
精确归档 ELF 的符号解析把调用定位到 `map<cmd_t*, NVNcommandBuffer*>` 插入；
`nvnCommandBufferInitialize` 未锁住全局表修改，且该表没有读取者。APK43 删除无用表，
同时给命令初始化和纹理 builder 默认值（也修改共享 map）补齐前端锁。
`apk42-crash.txt`、`apk42-stack-symbolized.txt` 保留归因；旧版短压力检查未重现竞态，不能宣称确定性复现。
`vulkan-test26.log` 通过八线程 4096 组 builder/命令生命周期及全部 GPU 检查，仍需真实游戏回归。
APK43 SHA-256：`ca3f8fc97753769b5d9ea2543c33b6fe6acb0dda9e9d24420956f6db540200a7`。

## 2026-10-24：内存服务与 Vulkan 实测（此前检查点）

本轮只运行 BOTW 1.6.0，没有推进 TOTK。原始 main Build ID 仍为
`8e9978d50bdd20b4c8395a106c27ffde`，SDK Build ID 仍为
`fcf93a41b08c5193a1410d262602dc621617c56f`。没有更换模块清单、游戏文件或启用新的 guest patch。

- Mac 16 KiB 页宿主：共享底座与新增内存测试 **2/2 通过**。Odin3 4 KiB 页宿主：
  独立内存测试通过，覆盖真实预留、预算、重叠页、清零、栈写回、锁定源和权限。
- 原始 BOTW SDK 成功完成 **16 次启动服务调用**，越过 ASLR/栈/别名区/内存量查询与首个
  loader 通知，停在 `ConnectToNamedPort`（SVC `0x1f`，SDK 偏移 `0x37eb50`）。
  独立探针保留 `blocked` 和退出码 3；仍未执行模块构造函数、`nnMain` 或游戏。
- 原生 NVN 前端注册 **113 个入口**，链接固定版本 D2R 独立 Vulkan 引擎。
  Odin3 / Adreno 830 上 **4 次提交、3 个离线测试 shader、1 个 draw、1 个 dispatch** 通过：
  2048 个 RGBA 像素、128 个裁剪像素、64 个 storage buffer 输出字逐项检查；
  同时验证 CopyCommands 的复制语义、GPU fence、读回和紧凑对象/视图边界。
- APK `0.4-memory-vulkan`（versionCode 4）已在 Odin3 `6edb0593` 原位安装。
  `assembleBotwDebug`、`assembleBotwDebugAndroidTest`、`lintBotwDebug` 通过。
  Instrumentation **4/4 通过且没有跳过**：模块导入/拒绝错误输入/重复原生会话、
  连续两次 GPU 会话、手柄会话、音频回调消费。没有清除应用数据。

APK SHA-256：
`0c0395019c281ad1ba485f423e74ef06d9303873bc55af8db89ce758ccf7ae67`。
本机 APK 为 `native-switch-study/porting-20261024/artifacts/BOTW-Android-memory-vulkan-arm64-debug.apk`；
原始日志同目录上级，分别为 `mng_startup_probe-memory-final.log`、`memory-probe-final.log`、
`vulkan-probe-final.log`、`apk-memory-vulkan-compute-tests.log`。

接口、依赖和明确限制见 [Vulkan 接入说明](../porting/nvn_vulkan/README.md)。测试 shader 完全由本仓库手写 GLSL 生成，
不能作为 BOTW shader 正确性或实际游戏性能证据。尚需接入服务管理器 IPC、线程/构造函数、
guest NVN 调用桥、BOTW shader 包及 Surface 呈现；当前 APK 仍不能游玩。

## 此前检查点（保留历史结果）

本文件记录较早的双游戏预览阶段。当前开发仅针对 BOTW，最新情况见[移植底座验证记录](../porting/VALIDATION.md)。

该阶段 BOTW/TOTK APK 已能导入准备好的模块，并在 Android 应用内执行原始 ARM64 SDK 函数。**两者均不能启动游戏或渲染游戏帧**，没有游戏运行、画面正确性或帧率结论。游戏程序、RomFS、着色器数据、密钥和存档均保留在 Git 与 APK 之外。

## 源码与依赖

Fork 为 `tencentmalos/MacNativeGames`，父仓库为 `ly05010419/MacNativeGames`。分支 `codex/android-zelda` 基于上游 `5281186`。Citron 只读参考检出未修改。Foundation/Oboe 使用 `dependencies.json` 中固定的干净源码提交；Foundation 提供输入和音频，本阶段无需修改它。Android 包含 GPL 组件，详见 `../THIRD_PARTY_NOTICES.md`。

## 可执行文件身份与准备

| 游戏 | 版本 | Main Build ID |
| --- | --- | --- |
| BOTW | Switch 1.6.0 | `8E9978D50BDD20B4C8395A106C27FFDE` |
| TOTK | Switch 1.0.0 | `082CE09B06E33A123CB1E2770F5F9147709033DB` |

六个原始 NSO 哈希均匹配已有版本清单。Android 重写器保留 Zelda 跳转表/独占访问窗口处理，使用 Bionic TLS 和已分配栈内临时区，并拒绝不支持的 SP 窗口。之前的准备验证通过了八项调用窗口差分测试，以及模拟 SP 以下内存被破坏的 192 个指令/种子用例。合成测试不能证明所有重写指令或完整游戏行为正确。

`package_modules.py` 独立读取各重写模块中的动态符号及 RELA/RELR 表，通过原始时钟导入定位 PLT，不移植其他游戏或 SDK 版本的地址。基于已验证报告重复打包，复现了两个固定清单哈希：

| 配置 | 清单 SHA-256 |
| --- | --- |
| botw160 | `30513eed53bc4ae9bd1539c9e28dbffdb859b9c2c054fefd05aa9e0de3054f38` |
| totk100 | `8878725de0e5776b4dbec4654dbaec1c5cb5484f395a24796e6ab7f2ffe898d0` |

APK 解析表之前先验证完整清单，再校验模块映像哈希，因此仅修改哈希声明无法让改变后的映像被接受。重写生成器更新时，必须一并审查、重新生成和验证配置固定身份。

## 应用内运行时验证

设备为 AYN Odin 3，Android 15，宿主页 4 KiB。两个调试 APK 均原位更新，保留应用数据和其他应用。各 flavor 的三项 instrumentation 测试均通过，包括未跳过的私有模块测试。

每个游戏在 APK 内均完成了以下验证：

- 导入、重定位并设置全部三个模块权限；解析强导入，报告置零的弱绑定，并通过 `/proc/self/maps` 验证每页实际权限。
- Guest 模块、栈和 TLS 均低于 `2^39`，不替换已有映射。代码 RX、可变数据 RW，宿主/guest 栈均有保护页。
- 使用两个标记发现 Bionic pthread-key 偏移，并在各工作线程交叉验证槽位。保留 Android 平台 x18，恢复宿主栈槽位。
- 四个原生线程通过原始 main PLT → SDK → 计时器跳板执行 4,000 次调用，要求结果单调且跳板计数精确。
- 每个线程调用原始 SDK `SleepThread` 等待 15 ms，要求出现一次正时长 SVC 等待且不能提前返回。宿主等待被中断时重试；调度让出变体和其他 SVC 尚不支持，不能伪造成功。
- 拒绝其他游戏配置、损坏清单、损坏 main、截断 SDK 和缺失文件。失败导入保留当前工作代次，只移除本次未完成副本。
- 再执行一次完整原生会话，验证 key/映射销毁和重新进入。
- 通过 Foundation 合成输入的会话、焦点和过期数据包检查，要求真实设备音频回调消费 PCM。这不代表实体按键覆盖或听感已验证。

界面提供目录导入、原生调用验证和报告分享。导入使用系统文档树 API 和应用私有暂存目录，原生验证成功后才原子发布选择。工作线程在应用范围内跨 Activity 重建保留，不与输入队列共用。事务导入和执行已在应用内测试。完整手动系统选择器授权/选择流程、导入时旋转屏幕**尚未完成**：选择器验证时设备前台切换到了其他应用，之后改为后台检查。

上述应用内路径不执行 `nn::init::Start`、DT_INIT 构造函数、`nnMain`、NVN 或 Vulkan，也未打开或修改游戏状态和存档。

## 独立 SDK 启动跟进

另行构建的 `mng_startup_probe` 可在带保护页的 guest 栈上进入原始导出 `nn::init::Start`。诊断代码仅以 `MNG_STARTUP_PROBE` 编译到独立可执行文件，JNI 库和 APK 均不暴露它。不支持的 SVC 输出结构化 `blocked` 报告并退出 3，输入无效退出 2，15 秒期限到达退出 4。它不会确认不支持的服务成功，也尚未执行模块初始化回调。

两个精确游戏配置都在 Odin 3 上以新进程运行。最先请求 GetInfo（SVC `0x29`）的信息 ID 11、子 ID 2 和 3。进程熵查询由 Bionic 系统随机源提供四个会话内稳定值，随机值不写入报告。之后原始 SDK 到达以下尚缺失的内存布局查询：

| 配置 | 下一个信息 ID | 现有宿主中的含义 | 原始 SVC 位置 |
| --- | ---: | --- | --- |
| botw160 | 12 | ASLR 区域地址 | sdk + `0x37ebc0` |
| totk100 | 4 | 堆区域地址 | sdk + `0x1584b0` |

这些偏移来自精确哈希校验的 SDK 模块实测，不是移入的补丁地址。报告包含 SDK Build ID、模块相对偏移、映射后的 guest 地址和输入寄存器。打包动态表中，两个位置都没有覆盖它的非零大小厂商函数符号，因此报告明确保留 unknown。入口名称来自原始厂商导出。错误游戏配置在执行前被拒绝。

回答这些查询前，必须让内存预留、已提交页记账与 QueryMemory 保持一致。探针目前停在此处，未到达首个加载器回调、构造函数、`nnMain` 或渲染器，也未修改存档。这是启动阶段证据，不是启动通过。

在已配置 Android CMake 构建目录中构建可选程序：

```sh
cmake --build <android-cmake-build> --target mng_startup_probe
adb -s <device> push <native-output>/mng_startup_probe /data/local/tmp/mng_startup_probe
adb -s <device> shell /data/local/tmp/mng_startup_probe <private-prepared-directory> botw160
```

该进程不依赖应用前台。报告及全部准备好的游戏数据均保留为私有本地测试产物。

## 构建检查与本地产物

- 使用 Xcode 26 的上游 Python 测试集：**332 项通过**。
- 使用 JDK 17、Gradle 9.8.1、AGP 9.4.1、SDK 36、NDK 29.0.14206865 构建两个 APK 及对应 instrumentation APK。
- 两次 lint 均零错误通过；剩余警告包括更新的工具链版本、仅支持 ARM64，以及开发界面的文本/图标完善项。
- APK 签名及 ZIP 16 KiB 对齐检查通过。APK 含 ARM64 原生库，无 `assets/` 条目。尚未在实际 16 KiB 页 Android 设备上执行，Odin 3 为 4 KiB 页。
- 本阶段未修改共享 Apple 运行时源文件。前一阶段分别为 Android、macOS、iOS 编译了共享混音器，并将 Apple BOTW/TOTK 重写结果与上游比较。本次仅针对 Android 加载器/应用，未重建完整 Mac/iOS 游戏包。

包名保持 `org.macnative.android.botw` 和 `org.macnative.android.totk`，当时 version code 为 2、版本名为 `0.2-runtime`，ARM64，最低 Android 12。以下已安装和归档 APK 对应已验证的 `dc80ba7` 阶段，后续独立启动诊断属于另一个可执行文件。

| APK | 字节数 | SHA-256 |
| --- | ---: | --- |
| BOTW-Android-runtime-arm64-debug.apk | 7,136,503 | `e1e847083f1939668e0a84d96fc8b45b1238395d43ce0a9ab13d1e31a1322620` |
| TOTK-Android-runtime-arm64-debug.apk | 7,136,503 | `3b9e3c72de9c1f43945c9c98434a4b017cd352c652c14557a7adc07317eefade` |

## 游戏运行前仍需完成的工作

1. 将原始 SDK 初始化及构造顺序接入完整 Android Horizon 线程、内存、同步和 IPC 服务。移植现有 Darwin 宿主依赖，不能用成功空实现掩盖缺失服务。
2. 接入 NVN 到 Vulkan，准备匹配反射/描述符布局的 Zelda 专用着色器包。D2R 游戏着色器不能替代 Zelda 着色器；离线 SPIR-V 仍需驱动专用 Vulkan 管线编译。
3. 接入 RomFS、私有存档、应用生命周期和 Foundation 游戏混音/输入。先输出真实游戏帧，再验证正确性、测量冷/热编译及同场景性能。

## 2026-10-24：启动设置、公共状态层与新 Turnip

APK65（0.7-overlay）在 Odin3 原位更新，SHA-256
`869f1df77eb907f980b6050774c82ad8b290a9e3ffda31fdcbf3abb6ee847584`。
默认 A/B 交换关闭；设置写入与重启保留已检查。Foundation 输入宿主测试通过。
公共状态层使用 Foundation OverlayShell/PerfHud，F10 打开 Control 后实测 RAM 开关
即时改变 HUD 并写入 overlay.json；触摸 HUD 顶部按钮仍存在未送达 JNI 的情况，尚未归因。

默认 Turnip 为 Mesa 26.3.0-devel（68dafb89a2），ELF SHA-256
`e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`。
APK 内驱动哈希及 DT_NEEDED 已检查，无 libc++_shared 依赖；游戏实际报告 Turnip/Adreno830。
应用自身的共享 C++ 运行时仍保留。

APK63 长时间标题界面退出，错误为 Texture snapshot limit。修复将录制副本的所有权
绑定到命令句柄、正在录制的命令及跨提交保存的 render-target 状态，并回收无人引用的副本。
66,000 次临时纹理录制超过旧上限，复制命令保留旧资源及原有 GPU 像素读回自检在新 Turnip 上通过。
这尚未解决 opaque-object registry 与引擎 image alias 表增长，不宣称全部纹理泄漏已经修复。

APK65 已再次加载复苏神庙存档，林克可见，但大面积拉长三角形仍存在。状态层观察到约
7 FPS、GPU 提交时间约 15 ms，仅为带状态层的现场读数；没有用于性能收益结论。
证据位于 Git 外 `native-switch-study/porting-20261024-gameplay/overlay65/`。

## 2026-10-24：高度纹理行距修复与 Litep 接入

本轮只推进 BOTW。Odin 3 使用同一 1.6.0 Build ID、静态 shader 包和无 libc++
动态依赖的 Turnip；没有改存档或重新导入游戏。APK72 的 SHA-256 为
`df4316c7cc8dc3b31a970290e0ddd3e4c8dc948859a9558af4fd4469c5c5eba2`。

坏帧 RDC65b 中，高度数组 R16 1024×1024×8 有规则条纹，经地形 VS 放大后，
在深度预通过中遮挡场景。进一步捕获高度数组的生产输入，发现实际 CPU-backed
R16 256×256 tile 使用 `flags=0x40`、`stride=512`。D2R 基线只将 `0x10`
视作 pitch storage，把该 tile 错当 block-linear。MacNativeGames `3c482a7`
和 Citron NVN texture_layout 均已将 `0x40` 单独视作线性布局。

Android 现在将该规则贯穿尺寸/对齐、上传、CPU ReadTexels/WriteTexels 和拷贝路径。
在构建目录适配固定 D2R 源码，原始参考仓库保持只读。新增 CPU 全图读回默认行距
处理。APK72 采到的两个 tile 上传结果分别与原始 pitch 字节完全一致；同一复苏神庙
存档已恢复人物、地面、柱子和门，随后转动镜头仍没有旧的全屏灰墙。证据目录为
`native-switch-study/porting-20261024-gameplay` 下 `source70`、`source72`、
`scene70b.png`、`scene72b.png`、`scene72d.png`。这关闭的是已定位的高度图布局缺陷，
尚未证明所有场景、光照和特效都与正确参考逐像素一致。

独立探针 `probe75.log` 在相同 Turnip 上通过 286 次提交、256 个同存储 GPU 别名、
12,288 个 pitched R16/RGBA8 GPU 采样像素，以及原有 BC、深度、SNORM、复制生命周期
检查。线性测试覆盖 0x10/0x40/0x50、自动和显式 padded stride、部分 CPU 写入、
padding 哨兵、CPU 读回及 GPU 采样。测试 shader 来自本仓库手写 GLSL，重新编译并
通过 spirv-val，不使用游戏 shader 作为单测输入。

同一物理 Image 的多个 NVN serial 不再重复进入地址区间索引，新增别名不再重建
整个索引，并去掉寻找 alias shared_ptr 的全表扫描。这没有解决全部对象表增长。
实机修复后 HUD 仍约 6–7 FPS；不能把消除重复工作直接宣称为已获得帧率提升。

底座已接入 Foundation Litep：默认关闭，8 MiB/线程 ring，异步 GPU submission、
upload prologue、present query，CPU coarse/detail 两档以及带 PID/SHA 校验的
调试服务与拉取工具。中文操作说明见 `../porting/PROFILING.md`。此前 APK67 的
ring 解码失败、scene 文件丢 1 块，均保留为失败证据，未用于性能收益结论。
APK70 的首次连续文件采集完整解码、28 块无丢块，能还原 GPU pair；该窗口含加载
过渡，且扩展校准后的 GPU/CPU 时间范围不一致，不能据此分析跨时钟依赖。
APK75 改为 Foundation fence midpoint 校准，并增加每帧 shader/pipeline/upload
增量计数。重复模式切换与 ring 留存边界仍须检查诊断后再接受为性能证据。

## Odin 3 默认手柄映射修正（APK 76）

Android 的 A/B/X/Y keycode 经 Foundation 表示为 South/East/West/North；
原适配器再次按物理方位映射为 Switch B/A/Y/X，导致用户设备的两对按键颠倒。
现在默认保持 A→A、B→B、X→X、Y→Y。Foundation 本身不变，
启动选项“交换 A/B”仍默认关闭，仅在用户开启时额外交换 A/B，不影响 X/Y。

Odin 3 上 `mng_input_tests` 已通过四个面键独立按下/松开、A/B 开关、
多键、失焦、断开重连及会话隔离。APK 76 已覆盖安装，未清除设置、存档或缓存；
SHA-256：`334dc7480d74806b00d1a2ede38a2412e41731390e101f9c6b8c46bfecb855c3`。

## 纹理地址索引优化（APK 78）

APK 76 的 Litep 文件采集显示：复苏神庙静止镜头每帧重建地址索引 8 次，
平均独占 13.421 ms。之前重建遍历所有 NVN serial，再按物理 Image 去重；
即使释放未创建 GPU 图像的 texture 或仍有其他 serial 引用的别名，也会标脏。

APK 78 用 `Image* → registered serial count` 保存物理图像集合。新增/移除别名
不会改变物理地址索引；仅新增图像或最后一个注册 serial 释放时标脏。计数不包含
GPU 在途退休对象的 shared_ptr 引用，因此不会把已注销但等待 GPU 的对象重新用于别名。
上传、同步、GPU 内容和退休时机不变，原 D2R 检出保持只读。

同一 Turnip 的 `probe78.log` 通过：286 次提交、23 次 draw、256 次 GPU 内容别名、
12,288 个 pitched R16/RGBA8 像素与原有生命周期/格式测试。Python 13 项测试通过。
APK SHA-256：`bc5987b51da6afcc986c82790fc7f06051deef1979b340c435ef3259aec467cd`。
同存档实机已进入复苏神庙，画面未重新出现灰色立面；性能复测结果另记。

APK 78 后续 8 秒文件采集（SHA-256 `3ab85eb0423e94fb88bd401d53c5295d738d0abd980abf31122e411630f27609`）
40 个 chunk 无丢块，63 个完整 present 窗口，平均 123.514 ms、P50 120.827 ms。
索引重建仍为每帧 8 次，但独占开销从 13.421 ms 降至 0.647 ms；Draw 为
32.994 ms，Fence 为 18.092 ms，未插桩部分 62.047 ms。索引优化确实消除了
按历史别名扫描的大部分开销，后续仍需细分 Draw 和未插桩部分。

注意：`scene78-capture-before.png` 显示人物朝向及镜头与 APK 76 不同，
所以 134.865→123.514 ms 不能作为严格同镜头的整体 FPS 提升结论。
Litep 首次打开时传入的 `scene_id` 包含 camera-default，是先于截图复核的
调用方标签；以截图为准，此差异同时写入本地 manifest，禁止将两个捕获直接合并。
GPU 保留边界不完整区间诊断，CPU/GPU 精确时钟对齐仍未知。

## 2026-10-24：Foundation debugbus 与 Litep CPU/GPU 直接接入（APK 79）

本轮仅修改 MacNativeGames fork 的 Android 原生底座，没有修改 Citron、
Foundation 或 D2R 的只读参考树。APK SHA-256：
`8937595c2c20a2743d7ad4ab694036c485b15cc38c1cb69c692d3eaace703c5e`。
已通过 `adb install -r` 安装到 Odin 3，保留模块、资源、设置、存档和缓存。
验证期间未采截图；通过 loader 身份、debugbus、实际 present/绘制计数和 Litep
确认新代码执行。重新启动后的镜头没有做图像核验，不能与 APK 78 作严格性能 A/B。

- Foundation 统一命令注册表已接入，支持 identity、模块/patch、线程、渲染缓存、
  覆盖范围和原有 profiler 命令；5 类实机查询含 ADB 往返约 31–52 ms。
- CPU 新增 SVC、Guest 等待/虚拟核获取、回调和可选全部 HLE 导入调用；
  HLE 汇编在整数与 SIMD 参数/返回寄存器之间保持 ABI，1024 次真实桥接调用
  覆盖记录开启/关闭和 HLE 开关组合，测试通过。线程表忙时返回可重试状态。
- GPU 在 32 个 Vulkan 工作命令调用点直接写 timestamp，支持每 present 预算和
  draw 采样间隔；真实 pipeline 创建单独命名。测试后高频模式全部关闭。
- 原有 Vulkan 回归通过：286 次 submission、23 draw、1 dispatch、纹理别名、
  pitched R16/RGBA8、66000 次纹理录制生命周期等均通过。Python 13 项通过。

原始证据保留在本机
`/Users/bytedance/workspace/native-switch-study/porting-20261024-bottleneck`：
`basic79`（基础）、`hle79`（高频 HLE）、`gpu79`（GPU 细分），PID 24158。
基础文件 SHA `46245139462327449f10917edfc299e5d10730ee0d6d0e47ba24d2d2c73ae0a9`，
47 个完整 present 区间平均 124.549 ms，P50 120.314 ms。
在 present 所在线程 TID 24239 上，条件变量等待平均 63.097 ms，
NVN.Draw 31.751 ms，NVN.Fence 17.685 ms，未覆盖约 0.110 ms。
这些是单线程经过时间，不能与其他线程等待相加；尚未确定条件等待的通知方。
48 组 pipeline/shader/compile 增量 counter 均为零。

高频 HLE 文件 SHA
`cafcc7c6b56c99a2af6ff228eb2c1d13f838a7ea9741001fcaa91407c045c662`，
23 帧平均 172.057 ms，明显包含采集扰动，不当作无探针性能。
TID 24245 的 `nvnSamplerCompare` 平均约 43097 次/帧、范围内经过时间约
15.899 ms；另有明显虚拟核等待。这是后续优化线索，不是已证明的关键路径。

GPU 文件 SHA
`5448fab3fd5b7eadcde58ab96562eb32e5104210144e0a714bac6a2a07e53b19`，
budget 512、draw stride 16。每 present 选择 257–287 个实际 GPU 命令，
细分拒绝数为零，bridge query 容量/无效 tick 丢弃计数为零。
Litep 解码得到 GPU.Draw 10751、GPU.Dispatch 705、GPU.Clear 1251 个完整区间；
该运行窗口未触发 copy/blit 细分，不能把源码接入等同于实机已覆盖所有路径。
三个文件均无 skipped chunks、无容器截断；保留启停边界未配对 CPU scope、
GPU orphan/incomplete zone 诊断。CPU/GPU 跨时钟对齐仍标作 unknown。

旧 APK 78 的长时间运行在约 7500 帧时触发 Native object retention limit；
完整日志保存在 `prior78-exit.log`。此问题尚未修复，不能宣称长时间运行稳定。
本轮交付的是可用的采集设施与实测定位，不宣称已经改善正常游戏帧率。

## 2026-10-25：60 FPS Guest Patch 与场景并发迭代（APK114）

已安装到 Odin3 `6edb0593`，`org.macnative.android.botw:game` PID 29957。
APK SHA-256：`0e65b652a76bf9108e46fafe3c4709952d251fca1475d99906a212d3980c2378`，安装后 base.apk 回读一致。
保持中文、720p handheld、1×、原 Turnip、`read_preflight=1`；没有清理用户数据或缓存。

- MacNative BOTW 1.6.0 的 VFR/UI/物理六子步/输入连发/影片间隔修正已默认开启；60 个指令点按 Build ID、原始身份、完整输入 SHA 和原指令验证。实际 Android 补丁输出通过 Unicorn 正反例，catalog 4 个单测通过。详情见 [REALTIME.md](../games/botw/guest_patches/REALTIME.md)。
- 标题基础窗口约 60 FPS；同存档加载流程进入场景后，本轮 6 秒基础窗口由 77.879 ms（12.84 FPS）到 67.507 ms（14.81 FPS）。温度、相机与动态分辨率没有完全固定，不能视为严格的单变量收益。60 FPS 补丁不保证场景达到 60 FPS。
- 录制/对象表与翻译分锁，Submit 保留不可变字节及纹理引用；固定 ABI 属性不等待翻译；Flush/Invalidate 等待前归还 Guest 虚拟核心。真实 GPU 同步顺序和图像回收仍保留。启用固定 D2R 的精确管线状态 memo，详细插桩默认关闭。
- 独立 `mng_vulkan_probe` 已显式重链接，在同代 Turnip、pipeline memo 开启时通过 305 次 GPU 提交、像素回读、百万对象压力、66000 次纹理录制回收，以及并发录制和旧命令 pin 的控制句柄复用检查。不能用 Gradle APK 构建代替独立 probe 的重链接。
- 实际游戏未做新一轮截图判读；没有依据本轮 GPU 测试宣布全部渲染异常消失。继续保留 scene 运行，HLE/detail/fine-GPU/ring 采集均已关闭。

性能证据和仍存约 16 ms/帧 GPU 围栏等待见 [OPTIMIZATION.md](../porting/OPTIMIZATION.md)。该历史检查点尚未接入 Foundation JobSystem（2026-10-26 已补 PSO 准备队列）；已对照 Azahar 的不可变输入、ticket、strand 与有界编译队列，接入顺序与验收边界见 [JOB_SYSTEM.md](../porting/JOB_SYSTEM.md)。

APK114 后续 20 秒基础文件排除可证明跨停录间隔的首 marker 后，连续 SDK frames 117–419 共 303 帧，平均 65.901 ms（15.17 FPS），最长 101.053 ms。排除原因与完整 SHA 见优化记录，仍存在场景性能瓶颈。

## 共享 Vulkan 字形视图修复（132-glyph-view）

Mac Vulkan 捕获确认 BOTW 字形 shader 的 2D-array 签名绑定到普通 2D R8 视图。
现参考 MacNative `tex_type_view` 补齐类型适配，并把 shader image flags 纳入采样缓存 key。
Mac 的复苏神庙实机已恢复“移动 / 操作说明 / 视角操作”；完整链及 GPU 像素回归见
[Mac 验证记录](../macos/VALIDATION.md)。此证据不等于 Android 实机通过。

Android BOTW APK 和独立 Vulkan probe 已重新构建。归档 APK SHA-256：
`84cf29acd8549d4902a24654206125c9acaff59e5bdb5068fa0c834ce6170c91`。
本轮 ADB 没有连接设备，未安装；未修改现有设备内容、配置和存档。
# 2026-10-26：共享 Vulkan 策略与 Odin3 实测

最新 APK SHA-256：`05f6fbad1580cbe7dfe71f7f4f0ac85373e3d99af856407d1f44f24420a76c85`。
Odin3 `6edb0593`，Adreno 830，Turnip `68dafb89`，720p 手持模式、内部 1×，
BOTW 1.6.0 main Build ID `8E9978D50BDD20B4C8395A106C27FFDE`。

- 用户确认复苏神庙静止场景：关闭录制后，30.105 秒、691 次成功 Present，**22.95 FPS**。
- 自动重新启动、读原最新存档并稳定后：30.126 秒、689 次成功 Present，**22.87 FPS**。
  每 5 秒均值 22.63–23.33 FPS。脚本只确认读档和稳定渲染负载，不推断精确地图或姿态。
- 指标为成功提交的原生 Present 节拍，不是 Android 合成器实际显示时间戳。
  后一次是新进程重复验证，不宣称严格的同进程 A/B 提升。

已将反馈快照、屏障合并、管线 memo、分批、呈现交接和诊断采样默认值统一到
`porting/nvn_vulkan/runtime_policy.h`。Mac / Android 都链接同一渲染器，平台层不再
分别设置这些默认值。生效状态见 `renderer_schedule status`。
相同场景日志的 Pass 数由首次 Android 配置约 465 降至约 130，但整体仍只有约 23 FPS。

4 秒 Litep 文件有 97 个真实 Present marker，96 次同步翻译平均 **25.025 ms**；
完整 Draw 适配路径的经过时间合计约 18.225 ms / 翻译，读预检查约 3.568 ms / 翻译。
Draw 包含资源准备、快照、状态查找和命令编码，不能解释为 Vulkan 驱动或 vkCmdDraw 的 CPU 成本。
多个 Guest 线程仍在前端锁上等待，不能把跨线程等待之和当作单帧时间或 CPU 使用率。
Host 呈现副本 Fence 平均 3.583 ms；它位于解锁的呈现线程上，不能直接与翻译耗时相加。
容器和载荷完整、无丢块或 CPU 配对错误，有一个 GPU 边界区间不完整；GPU/CPU 时钟对齐仍未知。

原始 PROF：`android-device-20261026/litep-policy/file-1792998149182608-1.prof`，
SHA-256 `bece30a2704e0bedc7c3e66f790f622aa2d9bf1669537f064af7a85764bb9def`。
证据根目录为仓库外的 `native-switch-study/android-device-20261026/`。

首次实机进程发生 `Image::shared_from_this` / `bad_weak_ptr`。已修复回收中写回重建索引后，
删除图像记录未再次标记索引失效的问题，保留原有 Fence 延迟销毁。负例失败、修复后回归通过；
Mac MoltenVK 和 Android Turnip 的完整 GPU probe 均通过。后续实机验证未再触发该异常，
不据短测试宣称所有长时间运行问题已解决。

自动验证脚本为 `android/tools/validate_scene.py`，复用 Mac 的 `autoload.c`。
它在打开存档数据后停止注入，真实用户输入会取消自动读档；没有存档时不按 A，避免误开新游戏。
缺失输入、卡在菜单、身份不符、退出或超时均保存结构化失败记录，无需反复询问场景状态。
构建和 lint 通过；默认策略、纹理索引、读档完成、无存档、用户接管和超时测试通过。


## 2026-10-26：翻译缓存与 CPU 占用口径补充

当前安装 APK SHA `a2ac1f0ee26db7f913fc4bd529f860610a4f847d83fdc1f0f87461d7b083a09c`，
PID 17101，已通过自动读档进入游戏负载，720p handheld / 1×，存档与缓存保持。
PSO 提前命中四窗口 A/B 仍约 22.2–22.6 FPS，没有明显整体增益；不将基础 Draw 的
18 ms 当作 vkCmdDraw 成本，也不将 Translate 的线程 CPU 98–99% 当作整机占用。
独立读取总 CPU 约 24.4%、GPU 设备 busy 约 28.5%，翻译线程约占一个核心的 60.8%。
详细证据、Mac / Android GPU 回归与后续瓶颈见
[NVN 管线缓存记录](../porting/nvn_vulkan/NVN_PIPELINE_MODEL.md)。

## 2026-10-26 PSO 准备队列

共享 Vulkan 已接入 Foundation WORK_JOB，任务按 key 去重并有序领取，完成 Mac/Android 构建和实际 GPU 回归。Odin3 同样 2255 个 PSO 的 2 / 4 worker 准备窗口为 60.6 / 41–47 ms；稳定场景仍约 22.5 FPS。原始 Litep、安装身份、默认值和未完成边界见 [PSO 验证](../porting/nvn_vulkan/PSO_WARMUP_20261026.md)。

## 发布解耦 W0（2026-10-27）

`tools/benchmark_publication.py` 限定 Odin3 `6edb0593`，要求明确的进程 PID、APK SHA-256
与 BOTW 1.6.0 Build ID。先用 `tools/validate_scene.py` 自动读档并确认稳定游戏负载，
再保持姿态采集四段各 30 秒的未录制窗口（诊断 off/on/off/on）。
随后单独录制 3 秒 Litep，开启已验证的 12 个 Guest 标签、地址等待和条件变量标签；
采样时间不能作为未插桩帧率基线。每段保留配置、热状态、呈现计数及锁等待表。
配置不一致立即保存字段差异并停止当前 A/B；结束时关闭采样和锁诊断。
同一阶段最多两轮，未达任务书验收门槛时停止，不靠增加轮次挑选结果。

W1 使用同一脚本的 `--mode publication`。每段未录制 FPS 窗口后单独取 3 秒 Litep，
只在 Litep 段开启 frontlock 诊断；记录开关前后队列计数，检查稳态 overflow 增量。
APK 继续原位安装并比对设置/缓存/存档哈希。新队列默认 0，最终状态恢复为测试前值。

## 2026-10-27 W1 实机验收结果

发布队列两轮验证后，最终同进程 ABAB 为 21.129 → 27.993 FPS（+32.49%）。
尚未满足 W0 基线的 P 锁等待门槛，唯一 identity 退休对账也未闭合；按任务书两轮上限停止。
`publication_queue` 默认及最终运行状态仍为 0；TextureCompare 去锁同样受此开关控制。
Mac/Android Vulkan 共用实现；构建、lint、26 项 CTest 与两端三模式 probe 通过。
详细报告：`porting/nvn_vulkan/ODIN3_PUBLICATION_W1_20261027.md`（相对仓库根目录）。

## 2026-10-27：切换 Thor，安装前 probe 未通过

`tools/benchmark_publication.py` 现显式支持 Odin3 `6edb0593` 和 Thor `9c2841a4`，
并按所选序列号检查对应型号；PID、APK 和 main Build ID 校验保持。
本轮 Thor / Adreno 740 使用同一 Turnip SHA `e61a373f…`，publication=0 / ordered=0
probe 返回 1，原因是 GPU report 与 Guest 时钟对齐检查失败。
未安装待验证 APK，现有包 SHA 仍为 `8656c2de…`；应用和存档未改写，启动属性保持，
最终没有游戏或 probe 进程。安装前后 78 文件哈希流程尚未执行，不记作通过。
没有新的 Thor FPS 或 Litep 结果，默认开关仍关闭。
完整记录见 [Thor 验证报告](../porting/nvn_vulkan/THOR_PUBLICATION_20261027.md)。

## 2026-10-28：Odin3 原 APK 复测

Odin3 `6edb0593`，APK `15e75fcb…`，PID 24717，720p 手持 / 1×。
三模式 GPU probe 通过，未复现 Thor 的时钟检查失败。自动读取存档并通过稳定负载门槛后，
同进程 OFF/ON/OFF/ON 为 21.430 / 28.303 / 21.247 / 27.649 FPS，
合并 21.338 → 27.977 FPS（+31.11%）；四份独立 Litep 保留完整性与边界诊断。
启动时处理了无密码锁屏、Odin 设置浮层和未启动的诊断 Service，没有在场景中跑图。
没有重装或改渲染器；78 文件无新增/删除，仅 option.sav 被游戏正常改写。
最终实验队列、采样关闭，启动属性恢复，游戏留在场景；W1 未完成的验收项保持未完成。
详细记录见 [Odin3 复测](../porting/nvn_vulkan/ODIN3_PUBLICATION_RECHECK_20261028.md)。


## 2026-10-28：范围 Invalidate W2

[Odin3 范围 Invalidate W2：同进程 ABAB、10 组双平台 GPU 回归、78 文件保全](../porting/nvn_vulkan/ODIN3_INVALIDATE_W2_20261028.md)。共享开关默认 0；本次实机进程开启，启动属性已恢复。

## 2026-10-28：Profiler O2 与 S5 ADPF

[O2验证](../porting/nvn_vulkan/ODIN3_PROFILER_O2_20261028.md)通过：真实SDK微基准、
未录制诊断ABAB、两份Litep/Perfetto/SF联合采集；优化只减少记录开销，不冒充游戏提速。

[ADPF验证](../porting/nvn_vulkan/ODIN3_ADPF_20261028.md)：Odin3 APK `8c950743…`，
28项底座+3项profiler CTest、双平台六组GPU probe、Android构建/lint通过；
同进程四窗29.4370→30.0086FPS，+1.942%，低于预设门槛。300秒ON窗口只完成167.44秒，
BOTW EXIT_SELF/status0，后续前台为shadPS4；没有新崩溃证明，没有重启挑选结果。
独立Hint开关采集未执行，p99/实际频率对照缺失，默认OFF。启动属性已恢复，
78个文件无增删、仅option.sav正常变化，最终无游戏进程。中断窗口工具现保留结构化
前缀，5项Python测试通过。原始数据、备份和阶段diff路径均在上述报告中。
