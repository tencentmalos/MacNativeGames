# 公共性能状态层

`porting/overlay` 是原生移植宿主共用的面板，采用 Foundation 的
`OverlayShell`、`PerfHud`、声明式 Control 页面及 `perf_metrics`。
它不依赖 BOTW、TOTK、Horizon 服务或游戏地址。Android 的 Surface、手柄与电池
传输放在 `android/native` 和 `PortOverlay.kt`，游戏只提供统计与身份。

## 宿主接入

1. 使用宿主唯一的 ImGui context。`initializeVulkan` 接收已选定的 Vulkan
   instance/device/queue、交换链图片、标题、实际驱动身份、字体及配置目录。
   Vulkan 后端和游戏共用 volk dispatch，不能加载另一套系统 Vulkan。
2. 在最终合成后将图片从 `TRANSFER_DST_OPTIMAL` 转为 `COLOR_ATTACHMENT_OPTIMAL`，
   调用 `drawVulkan`，再转为 `PRESENT_SRC_KHR`。交换链必须支持 COLOR_ATTACHMENT。
   当前 Android presenter 已有每帧 queue-idle，字体更新沿用此完成条件；未来异步化
   presenter 时必须同时处理字体更新和 ImGui frame buffer 的 GPU 生命周期。
3. 成功呈现后调用 `presented(FrameSample)`。字段为缺失值时传 `nullopt`，不能以 0
   冒充读数。平台 pointer/key 进入 Foundation 的有界线程安全队列；控制面板打开时
   宿主阻断游戏输入，并在打开、关闭、失焦时释放原来按住的输入。
4. 控制选项写入宿主传入目录下的 `overlay.json`；先写临时文件再 rename。
   Foundation 设置以稳定 ID 保存，标签翻译或游戏名变化不会重置设置。

Android 点击顶部状态条切换简略 / 横向 / 纵向；“控制”打开显示选项，“详细信息”
查看渲染统计。键盘 F10 或能传给应用的手柄 Mode 键可打开/关闭控制面板。
控制页提供 FPS/CPU/GPU 曲线、内存、电池、功耗、温度、背景透明度、文字描边等，
渲染统计页提供帧间隔、GPU 耗时、帧间隔曲线及管线构建耗时开关。

## 统计含义

| 显示 | 来源与限制 |
| --- | --- |
| FPS | 成功 present 后调用 Foundation FrameRateCounter，500 ms 窗口；不是 Android vsync 回调次数 |
| 帧间隔 | 相邻成功 present 完成时刻之差，包含游戏与宿主等待、帧限速 |
| CPU 利用率 | Foundation 采样 `/proc/stat`；不可读时用同 UID 进程累计 CPU ticks / 在线核心数，详情注明来源。只有频率时不伪装成利用率 |
| GPU 利用率/频率 | Foundation 低频工作线程读取设备全局 KGSL/sysfs；不是游戏独占 GPU 利用率 |
| 进程 CPU 时间 | `CLOCK_PROCESS_CPUTIME_ID` 差值；所有线程合计，可大于帧间隔，不是墙钟“CPU 帧耗时” |
| GPU 帧 | D2R 引擎已有每个提交首尾 Vulkan timestamp，在 fence 完成后回收；提交区间合计，显示延迟 8 帧的编号，不包括最终合成/HUD |
| 呈现调用 | Acquire、最终合成、present、queue-idle 的墙钟时间，包含等待，不是 GPU 时间 |
| 管线构建 / Shader 加载 / 上传 | D2R 累计计数器逐帧差值；管线构建与 Shader 加载分开，不能把二者都称为 Shader 编译 |

设备数据默认每秒采样一次，不在每个 draw 调用读取 sysfs。帧间隔曲线固定 120 个样本，
Foundation FPS/CPU/GPU 曲线固定 30 个低频样本。缺失 GPU 查询显示 `--`。
状态层只能提供瓶颈线索，不能单靠利用率证明错误几何或长帧原因。

## 依赖与边界

Foundation 使用 `FOUNDATION_OVERLAY_HOST_IMGUI_TARGET` 的独立宿主入口，保持参考仓只读。
该 pin 的 perf CMake 依赖完整 Android platform，因此本宿主编译相同的两个 portable
源文件并提供 `BatterySource` 适配器；电池读数来自 Android BatteryManager / sticky intent。
没有伪造 `JniHelper` 或平台服务。

中文字体来自设备 `/system/fonts/NotoSansCJK-Regular.ttc`，使用 FreeType 解析 CFF2，
不把系统字体复制到 Git。Foundation 当前 vendored ImGui core 为 1.92.2b，但 FreeType
适配文件仍为旧接口；宿主使用匹配的官方 ImGui 1.92.2b 外部检出，避免修改 Foundation pin。
所有新增依赖固定在 `android/dependencies.json`，原始许可随 APK 保存。
Foundation 状态层与 metrics 的源文件声明仅供学习研究、不得再分发；当前 APK 仅作本地研究，
不能将它表述为可公开发布的 MIT-only 二进制。

当前 Vulkan 资源跟随专用游戏进程存活；Mac 宿主可复用同一个 Overlay/FrameSample 接口，
但尚未接入 Metal renderer，不宣称 Mac 图形后端已完成。
