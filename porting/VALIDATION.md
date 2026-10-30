# BOTW 移植底座验证记录

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

接口、依赖和明确限制见 [Vulkan 接入说明](nvn_vulkan/README.md)。测试 shader 完全由本仓库手写 GLSL 生成，
不能作为 BOTW shader 正确性或实际游戏性能证据。尚需接入服务管理器 IPC、线程/构造函数、
guest NVN 调用桥、BOTW shader 包及 Surface 呈现；当前 APK 仍不能游玩。

## 此前检查点（保留历史结果）

日期：2026-10-24。源码基线为用户 fork 的 `codex/android-zelda` 分支上 `c7fcac6`，加上本阶段修改。本阶段仅推进 BOTW 1.6.0，未重建、安装或使用现有 TOTK APK 验收。

## 已实现与已验证范围

| 检查 | 结果 | 边界 |
| --- | --- | --- |
| 仓库 Python/原生测试集 | 341 项通过，耗时 61.235 s | 包含重新编译的公共核心、清单身份/依赖失败路径及解析器反例 |
| BOTW 清单 | 12 项；两端私有清单均匹配原始模块固定身份 | 仅为迁移清单，启用的公共可执行补丁为零 |
| Odin3 公共核心 | 通过 | 错误身份、原字节不匹配、重叠、分类开关、原子失败、应用记录及调试所有权/代次 |
| Mac BOTW 可执行文件 | 以警告视为错误的配置构建；原生探针通过 | 四线程完成 4,000 次原始 main PLT/SDK 调用；未执行构造函数或游戏入口 |
| Android BOTW 原生探针 | 通过 | 四线程、独立 guest 栈/TLS，完成 4,000 次原始调用和四次 SDK 等待 |
| 原生元数据/解析器 | Mac/Android 四份真实加载器快照通过 | 原始 SDK Start 厂商符号、精确跳板返回映射、重写区内部地址明确返回 unknown |
| Mac 原生调试器 | checkpoint 断点及全停状态下稳定描述符读取通过 | 使用合成测试程序，未验证 guest 栈重建或完整游戏调试 |
| BOTW APK 构建与 lint | 通过 | 仅运行 `assembleBotwDebug` 和 `lintBotwDebug` |
| 已安装 APK 测试 | 3 项通过，无跳过 | 私有模块导入/执行、失败导入保留、合成手柄事件、音频回调消费 |
| APK 签名/对齐 | 通过 | 调试签名和 ZIP 16 KiB 对齐；实际设备页大小为 4 KiB |

设备为 AYN Odin3，Android 15、ARM64、4 KiB 宿主页。没有使用 root、清除应用数据或卸载应用，现有私有输入和导入代次均保留。模块 instrumentation 测试以状态码 0 完成，不是条件不满足导致的跳过。音频/输入测试不能证明实体按键布局或游戏音频听感正确。

上游测试集输出了子进程 `ResourceWarning`。完成后检查所报告的进程，均已退出，测试集返回成功。Android 构建中 Foundation 的 JSON 依赖仍有既存 Clang 字面量运算符弃用警告，本次未修改依赖源码。

工具链：`/Applications/Xcode.app` 中的 Xcode、JDK 17、Android SDK/build-tools 36、NDK `29.0.14206865`、CMake `3.22.1`、Gradle `9.8.1`、AGP `9.4.1`。Foundation 和 Oboe 保持 `android/dependencies.json` 固定的提交。

## APK 身份

已安装包：`org.macnative.android.botw`，version code **3**，版本 **0.3-foundation**。调试 APK 不包含游戏模块、RomFS、密钥、存档或已翻译的游戏着色器。

SHA-256：

```text
3a6eaa0dcc0744336eeb7851e362fecda1982748a82c3f5d59a5130f56c187bd
```

本地产物：`/Users/bytedance/workspace/native-switch-study/porting-20261024/artifacts/BOTW-Android-foundation-arm64-debug.apk`。

## 原生调试器证据

通过现有 native-debugger MCP 使用有界合成测试程序，在导出的 `mng_port_debug_checkpoint` 上设置函数断点。成功会话为 `d03b0d7e4a3b4576bdc7d319ebc7b01d`，stop epoch **2**，全部线程停止。按“描述符 → 内存 → 描述符”读取，ABI 为 1、描述符大小 40、偶数序号 6、代次 1、数据长度 37 字节，两次描述符完全一致。数据标识了测试程序的 checkpoint，全程未调用目标辅助函数。恢复后进程以代码 0 退出，会话已清理。

测试程序 Mach-O UUID：`53A62419-29A1-3772-9227-CFE10CA49724`。完整 target token、会话日志和内存读取证据保留在私有采集中。这些身份只对应该次测试会话，不代表游戏可执行文件。

默认 Xcode 16.3 的 `lldb-dap` 首次附加失败；改用 `/Applications/Xcode.app/Contents/Developer/usr/bin/lldb-dap` 后成功。第二次短测试在安装断点前退出，因此在测试程序中加入有界附加窗口，完成上述读取。三次自建会话均已清理，包括附加失败的会话。正式运行时没有该等待，也没有强制终止调试器持有的游戏进程。

真实 Mac BOTW 加载器另外发布了公共格式元数据，并通过原生调用探针。Android 加载器输出 `modules_sealed`、`sdk_startup`、`startup_blocked` 快照；每份快照均绑定精确私有清单，并解析到原始四参数 SDK Start 导出。Android LLDB 附加、native-debugger provider 集成、完整重写来源映射、guest/宿主混合栈回溯和线程等待图尚未实现或验证。

## 当前运行边界

Android 独立启动探针在完成两次进程熵查询后，仍停于 `GetInfo` 信息 ID 12（ASLR 区域地址），报告 **blocked** 并以代码 3 退出。构造函数、`nnMain`、NVN Vulkan 帧和游戏逻辑均未运行。该 APK 是运行时预览版，尚不能游玩 BOTW。本阶段没有 Android 实际游戏帧率或着色器性能对比结果。

Mac 探针保留并检查了现有补丁行为，观测到正文中 18,249 处、重写区中 63 处独占读升级。这些数量只对应精确 Mac 输入；Android 重写必须独立枚举并验证，包括并发行为。

BOTW 清单记录兼容性、时序/输入、图形和诊断适配。Mac 现有的重定位后补丁尚未迁入 `apply_staged`。静态补丁、依赖基址的修正和状态回调都需分别证明后才能启用。Mac 调试快照仍明确标记逐补丁应用记录不可用。

本次未进行完整 iOS 构建，新增 Mac 调试适配器在 iOS 上排除。完整 Mac 游戏/应用打包和实际游戏运行也不是本阶段验收项；重新构建的是 BOTW 原生加载器可执行文件。

私有证据保留在 Git 之外的 `/Users/bytedance/workspace/native-switch-study/porting-20261024`，包括 `upstream-tests-final.log`、`porting-android-test-final.txt`、`botw-apk-tests-final.txt`、`botw-mac-snapshots-final.log`、`botw-android-startup-snapshots-final.log`、`native-symbol-validation-final.json`、`botw-apk-artifact.json` 及三份调试会话导出。游戏输入和生成清单不属于仓库产物。
