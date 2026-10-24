# Android 运行时阶段记录：2026-10-24

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
