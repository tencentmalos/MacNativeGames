# BOTW / TOTK Android 移植

开发分支为 https://github.com/tencentmalos/MacNativeGames 中的 `codex/android-zelda`，基于上游 `5281186`。fork 已存在，修改前核实了其父仓库关系。Citron 只读参考检出未修改。

实测结果与待完成的运行时工作见 [验证记录](VALIDATION.md)。

当前**只推进 BOTW 1.6.0**，版本为 `0.3-foundation`。现有 TOTK 预览保留在 `0.2-runtime`，开发循环不要同时构建两个 flavor。Mac/Android 公共底座设计、补丁策略和调试约定见 [移植底座](../porting/README.md)，当前证据见[底座验证记录](../porting/VALIDATION.md)。

## 当前能力边界

两个 APK flavor 均为**原生运行时预览**，尚不能游玩。它们加载用户准备的模块，通过 Android 跳板运行原始 SDK 函数，并接入 Foundation 手柄采集、InputHub、JNI 桥接和 Oboe 输出。界面明确标示游戏尚不可运行。APK 不包含游戏模块、密钥、存档、RomFS 或翻译后的着色器。

已实现：

- Android/Bionic TLS 和无 red zone 指令重写，保留 Zelda 跳转表发现及关联独占访问窗口处理。遇到不安全 SP 情况时失败，不退回到 SP 以下临时存储。
- Android 安全的 BLR x18 窗口保留原返回 PC。Apple 默认流程保留；Android 映像布局带 TARGET_ANDROID，Apple 表生成器会拒绝它。
- 私有目录导入和原子选择代次记录。导入失败保留原始来源和上一工作代次。
- SHA-256 固定的 Android 清单与模块映像、模块重定位和导入解析、RX/RW 分离、带保护页的栈、逐线程 Bionic TLS 验证。Guest 映射处于 Horizon 39 位地址范围内，分配器不覆盖现有映射。
- 四个独立 guest 栈上的原始 main PLT → SDK 计时器调用，以及原始 SDK `SleepThread` → 宿主等待。不支持的服务跳板会停止，不能伪造成功。校验/导入不执行 guest 代码。
- Foundation Android 手柄采集与原生 InputHub，按物理按键位置映射 Switch FullKey。原生会话 token 和连接代次拒绝过期输入；失焦/暂停结束输入会话并释放按键。
- Zelda 混音器已有 Foundation 输出分支。生产线程将有明确所有权的 PCM 交给 Foundation 回调混音器，Oboe 实时回调不调用 guest 混音器。关闭时取消待提交数据并等待线程结束，再释放混音上下文。APK 构建会编译真实混音器的 Android 版本，但完整 guest 运行时尚未链接到 APK。

尚未实现原始 SDK 初始化/构造函数、完整 Horizon 线程/内存/IPC 服务集成、NVN 到 Vulkan 的接入、Zelda XGS 生成、游戏生命周期/存档/导入、振动分发、陀螺仪和屏幕触控。当前原生适配器不宣称支持触觉反馈，也没有 Android Zelda 帧率结果。

可选的**独立** `mng_startup_probe` 能进入原始 SDK Start 导出，报告首个不支持的服务。它不打入 APK，也不通过 JNI 暴露。15 秒进程期限及不支持操作时的非零退出码，将部分启动与游戏启动成功区分。首个已实现启动查询为进程随机熵；Odin 3 上后续请求分别为 BOTW ASLR 范围、TOTK 堆地址。详见 `VALIDATION.md`。

## 构建

使用 JDK 17、SDK 36、NDK 29.0.14206865 和 SDK CMake 3.22.1。按 `dependencies.json` 获取精确源码提交，Foundation 需要用户仓库访问权限。CMake 拒绝不匹配的检出。直接使用依赖源码模块，不复制或修改 Foundation。

```sh
cd android
./gradlew :app:assembleBotwDebug :app:lintBotwDebug \
  -PfoundationDir=/absolute/path/to/foundation \
  -PoboeDir=/absolute/path/to/oboe
```

包名为 `org.macnative.android.botw` 和 `org.macnative.android.totk`。均要求 Android 12 或更高版本及 ARM64。wrapper 固定 Gradle 9.8.1 和发行包 SHA-256。Foundation Kotlin 源码使用 AGP 9 的 [AndroidSourceSet.kotlin API](https://developer.android.com/build/migrate-to-built-in-kotlin)。Android 依赖许可见 `THIRD_PARTY_NOTICES.md`。

```sh
./gradlew :app:connectedBotwDebugAndroidTest \
  -PfoundationDir=/absolute/path/to/foundation \
  -PoboeDir=/absolute/path/to/oboe
```

Instrumentation 测试提交合成手柄事件，并播放低音量的 250 ms 测试音，要求 Foundation 设备回调实际消费 PCM。这不能证明实体手柄映射或可听输出质量。

## 在私有目录准备游戏模块

使用 Python 3.11+ 运行 `games/totk/android/prepare_modules.py --help`，指定安装 Capstone 和 LZ4 的 Python 解释器。输入必须是原始 BOTW 1.6.0 与 TOTK 1.0.0 ExeFS 目录，不能使用已经重写的映像。现有工具在写入前按版本清单验证全部六个 NSO。输出必须是 Git 之外的新目录。每项结果记录模块 Build ID、完整输入哈希、重写器/调用窗口代码哈希、输出哈希及退出状态。任一模块失败都会保留报告并使命令失败。生成成功不代表语义等价、启动成功或游戏验证通过。

为各 APK 打包已验证的重写结果，输出仍必须是 Git 外的新目录。当前仅推进 BOTW；TOTK 命令保留作历史流程参考：

```sh
python games/totk/android/package_modules.py --rewrite-report /private/rewrite/report.json \
  --title botw160 --output /private/modules/botw160
python games/totk/android/package_modules.py --rewrite-report /private/rewrite/report.json \
  --title totk100 --output /private/modules/totk100
```

打包器从精确模块中提取重定位、符号和时钟 PLT。`expected-modules.json` 固定清单、源哈希、Build ID 和生成器指纹。生成器变化后，必须重新准备并完成实机验证才能更新固定身份；仅改清单中的声明哈希无法让 APK 接受修改后的映像。

将准备好的目录复制到设备，在对应应用中选择 **Import prepared modules**。这是四文件模块包，不是 XCI/NSP 导入器或 RomFS/着色器包。导入在 UI 线程之外运行，并可跨 Activity 重建继续。**Verify native runtime** 执行 4,000 次计时器调用和四次 15 ms SDK 等待，不启动游戏。最近一次界面执行报告私有保存于 `files/last-runtime-report.json`，可分享为文本。

已导入模块或在应用 `files/test-input` 下私有提供模块时，设备测试会覆盖导入、损坏拒绝、错误游戏拒绝和重复原生会话。缺少私有模块时会明确跳过模块测试，手柄/音频测试仍执行。

合成差分测试中 Unicorn 仅用于测试：

```sh
python games/totk/scripts/test_call_window.py
python games/totk/scripts/test_android_rewrite.py
python -m unittest discover -s tests
```

上游 Apple 测试使用 Xcode 26，旧 Xcode 16 SDK 无法编译其中一个 MetalFX 测试。本 Android 阶段没有重建完整 Mac/iOS 游戏包；共享混音器已分别为两个 SDK 编译。BOTW 原生加载器的新验证见前述底座记录。
