# BOTW / TOTK Android 移植

开发分支为 https://github.com/tencentmalos/MacNativeGames 中的 `codex/android-zelda`，基于上游 `5281186`。fork 已存在，修改前核实了其父仓库关系。Citron 只读参考检出未修改。

实测结果与待完成的运行时工作见 [验证记录](VALIDATION.md)。

当前**只推进 BOTW 1.6.0**，版本为 `0.5-native-startup`。现有 TOTK 预览保留在 `0.2-runtime`，开发循环不要同时构建两个 flavor。Mac/Android 公共底座设计、补丁策略和调试约定见 [移植底座](../porting/README.md)，当前证据见[底座验证记录](../porting/VALIDATION.md)。

## 当前能力边界

BOTW APK 已链接完整 SDK 启动链路和原生游戏进程，仍属于开发验证版本。TOTK 保留旧预览，不在本阶段验证。APK 不包含游戏模块、密钥、存档、RomFS 或翻译后的着色器。实际场景进展见验证记录，不能把启动或首次 Present 当成进入游戏成功。

已实现：

- Android/Bionic TLS 和无 red zone 指令重写，保留 Zelda 跳转表发现及关联独占访问窗口处理。遇到不安全 SP 情况时失败，不退回到 SP 以下临时存储。
- Android 安全的 BLR x18 窗口保留原返回 PC。Apple 默认流程保留；Android 映像布局带 TARGET_ANDROID，Apple 表生成器会拒绝它。
- 私有目录导入和原子选择代次记录。导入失败保留原始来源和上一工作代次。
- SHA-256 固定的 Android 清单与模块映像、模块重定位和导入解析、RX/RW 分离、带保护页的栈、逐线程 Bionic TLS 验证。Guest 映射处于 Horizon 39 位地址范围内，分配器不覆盖现有映射。
- 四个独立 guest 栈上的原始 main PLT → SDK 计时器调用，以及原始 SDK `SleepThread` → 宿主等待。不支持的服务跳板会停止，不能伪造成功。校验/导入不执行 guest 代码。
- Foundation Android 手柄采集与原生 InputHub，按物理按键位置映射 Switch FullKey。原生会话 token 和连接代次拒绝过期输入；失焦/暂停结束输入会话并释放按键。
- Zelda 混音器已有 Foundation 输出分支。生产线程将有明确所有权的 PCM 交给 Foundation 回调混音器，Oboe 实时回调不调用 guest 混音器。关闭时取消待提交数据并等待线程结束，再释放混音上下文。APK 已链接真实 guest 音频渲染器和混音器；可听输出质量仍需游戏验证。

内存、线程、IPC、RomFS/DLC、账户/存档和字体服务已连接原始 SDK；NVN 调用经过保持 Bionic TLS、x18 与宿主栈的跳板进入 Vulkan。详细边界见 [Vulkan 接入说明](../porting/nvn_vulkan/README.md)。Android Surface 负责真实呈现，Foundation 负责手柄和音频设施；振动、陀螺仪、触控与完整退出生命周期仍未完成。

`mng_startup_probe` 与 APK 共用启动代码。探针无窗口，到首次 Present 时明确停止；APK 在专用 `:game` 进程运行，失败不会通过 guest 汇编栈展开 C++ 异常。每次启动保留上一份原生日志。前台离开导致 Surface 销毁时，目前以结束独立进程收尾，尚未实现协作式关机；保存完成前不要切换应用。

## 私有游戏资源与离线 shader

完整游戏构建额外传入 `-PbotwAssetConfigDir=/private/verified/botw160`，目录包含已有 Mac 准备流程生成的 `romfs_config.h`、`aoc_config.h`，父目录包含 `shared_fonts_config.h`。未提供时仍可构建独立平台/模块预览，不能启动完整 BOTW。

通过 **Select BOTW asset folder** 授权目录，布局为 `romfs/`、`aoc/1/`、`aoc/2/`（各有 `romfs.bin` 和 `runtime-index.bin`），以及 `shared-fonts/` 与 `shaders/`。大型 RomFS 通过持有的 SAF 文件描述符访问，不再复制一份游戏镜像。原生运行时校验已知内容哈希并复用文件身份记录。

离线编译示例（输出必须在 Git 之外）：

```sh
python android/tools/prepare_botw_shaders.py \
  --romfs /private/verified/botw160/romfs \
  --modules /private/rewritten/botw160 \
  --mac-seed /private/mac-app/shader-seed \
  --d2r /absolute/path/to/pinned/d2r-mobile \
  --output /private/new-botw-vulkan-bundle
```

将输出 `bundle/manifest.json` 和 `bundle/bundle.zip` 放入授权目录的 `shaders/`。APK 通过一个 SAF 流导入归档，逐项检查条目和哈希，避免逐个打开上万个共享存储文件；也兼容 `files` 列出的独立 XGS 文件。
工具保存输入身份、变体和验证报告；部分失败时退出码非零，缺失项不能当成完整游戏覆盖。
MacNativeGames 翻译器生成 SPIR-V，未复用 Metal 机器码。键包含控制段和机器码，
多变体只有全部通过且产物相同才合并。APK 按 manifest 摘要隔离缓存，逐文件校验 SHA-256。

启动按钮进入原生 BOTW。存档在应用 `files/native-botw160`，与其他包隔离；开发验证使用
用户 Mac 存档的独立副本，原始存档保持不变。手柄接入 Foundation；键盘 J/K/I/U
对应 Switch A/B/X/Y，Enter/Tab 对应加减键，WASD 为左摇杆方向。

## 构建

NVN→Vulkan 的实现和默认策略与 Mac MoltenVK 共用，详见
[共享 Vulkan 策略](../porting/nvn_vulkan/RUNTIME_POLICY.md)。Android JNI 只负责平台输入，
不维护另一组渲染器默认开关；实机验证用 `renderer_schedule status` 核对生效值。

## 自动进入存档并测帧率

已导入模块、资源和存档的 Debug APK 可直接运行：

```sh
python3 android/tools/validate_scene.py --serial DEVICE --restart \
  --output /private/new-validation-directory
```

`--restart` 仅结束这个 BOTW 包的现有进程，不清除数据。脚本通过 Debug 启动参数进入游戏，
临时启用共享 `autoload.c`，打开最新存档后停止按键，等待稳定的场景渲染负载，再记录 30 秒
成功 Present。默认等待上限 180 秒，稳定窗口 10 秒；真实用户输入会停止自动读档。
没有存档时不会误启动新游戏。脚本不以菜单 FPS 作为场景结果，也不假定读取存档等于加载完成。
它校验原生进程、BOTW Build ID、前台 Activity 和录制状态，保存事件、日志与结果，退出时恢复
`debug.mng.autoload`，保留健康游戏进程。精确地点和人物姿态不是渲染负载阈值能自动证明的。

## 编译命令

使用 JDK 17、SDK 36、NDK 29.0.14206865 和 SDK CMake 3.22.1。按 `dependencies.json` 获取精确源码提交，Foundation 需要用户仓库访问权限。CMake 拒绝不匹配的检出。直接使用依赖源码模块，不复制或修改 Foundation。

```sh
cd android
./gradlew :app:assembleBotwDebug :app:lintBotwDebug \
  -PfoundationDir=/absolute/path/to/foundation \
  -PoboeDir=/absolute/path/to/oboe \
  -Pd2rDir=/absolute/path/to/d2r-mobile
```

包名为 `org.macnative.android.botw` 和 `org.macnative.android.totk`。均要求 Android 12 或更高版本及 ARM64。wrapper 固定 Gradle 9.8.1 和发行包 SHA-256。Foundation Kotlin 源码使用 AGP 9 的 [AndroidSourceSet.kotlin API](https://developer.android.com/build/migrate-to-built-in-kotlin)。Android 依赖许可见 `THIRD_PARTY_NOTICES.md`。

设备测试先构建 `:app:assembleBotwDebugAndroidTest`，再原位安装两个 APK，并直接调用：

```sh
adb -s DEVICE install -r app/build/outputs/apk/botw/debug/app-botw-debug.apk
adb -s DEVICE install -r app/build/outputs/apk/androidTest/botw/debug/app-botw-debug-androidTest.apk
adb -s DEVICE shell am instrument -w \
  org.macnative.android.botw.test/androidx.test.runner.AndroidJUnitRunner
```

不要在保存了用户数据的验证包上直接运行 Gradle `connected…AndroidTest`：本机 AGP/UTP
在测试结束时自动卸载目标包。直接 instrumentation 调用保留目标应用及数据。

**Verify Vulkan rendering** 使用独立缓存目录中的手写 shader，验证实际 Vulkan 清屏、绘制、计算、GPU 读回和资源关闭；不启动游戏，不使用 BOTW shader。

Instrumentation 测试还会连续运行两次 Vulkan 会话，提交合成手柄事件，并播放低音量的 250 ms 测试音，要求 Foundation 设备回调实际消费 PCM。这不能证明实体手柄映射或可听输出质量。

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

## 启动设置与公共状态层

BOTW 首屏提供 GPU、语言及游戏文件入口。默认选择与 shadPS4 同版的 Turnip，
语言为简体中文；选择会传给下一次启动的独立游戏进程。
设置中的“交换 A/B 键”默认关闭，只交换游戏内手柄最终快照的 A/B 位，
X/Y、键盘、启动器确认键保持原映射。按下、释放、同时按键与重连共用同一映射。

性能面板采用 Foundation，接入和数据含义见 [公共状态层](../porting/overlay/README.md)。
显示控制与统计逻辑放在 `porting/overlay`，`PortOverlay.kt` 只传输平台输入和电池读数。

构建新增外部依赖参数 `-PvolkDir=... -PadrenotoolsDir=... -PfreetypeDir=...`，
以及匹配 `android/turnip-lock.json` 的 `-PturnipDir=...`。ImGui 的匹配版本通过
`-PimguiDir=...` 指定。fmt 默认取 Foundation 同级的 `fmt` 检出，并校验固定 commit。
BG/icon 可用 `-PlaunchArtworkDir=<EmuPSP Ghost 本地缓存目录>` 提供；没有引入下载、
登录或在线内容服务。素材是私有本地构建输入，不进入源码仓。

Turnip 已切换到静态隐藏 C++ 运行时的 ELF：Mesa commit 仍为 `68dafb89a2`，
新 SHA-256 为 `e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`。
它没有动态 libc++/libc++abi/unwind 依赖；宿主和 hooks 仍使用同一个应用共享运行时。
构建先验证锁文件及二进制哈希，运行时验证 `VK_DRIVER_ID_MESA_TURNIP`，失败时明确报错。
旧驱动保留在各自产物/内容寻址缓存中，不覆盖已有诊断基线。

### 翻译与 UMA A/B

`validate_scene.py --existing` 可验证当前游戏进程，不再重复启动；新进程仍使用
`--restart`。门控在读档、暖机及前台均稳定后开始，采样中丢失前台会保存失败证据。
它不会把渲染负载门控当作精确地图识别。

```sh
python3 android/tools/benchmark_translation.py --serial DEVICE --output /absolute/new-directory
python3 android/tools/benchmark_memory.py --serial DEVICE --first-policy coherent --output /absolute/another-new-directory
```

前者对图像缓存、动态描述符 plan 做同进程 A/B；后者重启比较上传 arena 内存类型，
每种模式自动读原存档，独立采集无 profiler 的 Present 节拍及 Litep 文件。
省略 `--first-policy` 时读取当前 renderer 的实际值，不单独维护 Android 默认值。
结束恢复设备属性，失败过程和当时 PID 保留在结果内；失败后运行中的进程仍可能属于
中间配置，不能仅看已恢复的属性宣称运行模式已恢复。场景、温度和背景任务仍须核对。

`upload_cached` 默认为 0，保留已验证的 coherent-first 策略；可显式开启测试同一设备
上的 cached+coherent+device-local 类型。本次未测到明确增益，暂不据 UMA 推定 cached
上传更快。分析与原始证据索引见 [翻译和 UMA 记录](../porting/nvn_vulkan/ANDROID_TRANSLATION_20261026.md)。

`benchmark_translation.py --pipeline-only` 单独比较 PSO 提前命中，按关／开／关／开
保持同一进程，其余运行设置保持不变。每批 Litep 同时记录实际线程 CPU 时间与
管线提前命中／完整状态构建累计数，可核对减少的是准备工作还是仅移动等待。

### 并发 PSO 预热验证

```sh
python3 android/tools/benchmark_pso_warmup.py --serial DEVICE --workers 1 4 --seconds 20 --output /absolute/new-directory
```

脚本保存现有属性，依次启动匹配 APK，用首帧前 Litep 文件记录 PSO 任务，等待完整预热、
停止采集，再自动读档测量无 profiler 的帧率。结果包含 PID、APK 哈希、预热列表哈希、
实际并发数和完整 PROF/sidecar。不会清 driver cache 或游戏数据；这衡量已有 driver cache
下的准备吞吐，不代表冷 shader 编译。属性恢复只作用于下一进程，末次运行的 worker
数另外报告。输入相同还需核对列表哈希和场景，不能只比较脚本墙钟时间。
