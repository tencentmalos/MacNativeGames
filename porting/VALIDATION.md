# BOTW 移植底座验证记录

## 2026-10-24：内存服务与 Vulkan 实测（当前检查点）

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
