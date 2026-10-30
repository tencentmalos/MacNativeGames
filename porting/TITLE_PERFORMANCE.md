# BOTW 首屏的节拍与时钟修复

按用户要求，本轮停留在启动后的游戏首屏，不发送读档输入，不以场景内旧结果代替首屏验证。设备为 Odin3，BOTW Switch 1.6.0，主模块 Build ID `8e9978d50bdd20b4c8395a106c27ffde`；720p 手持、内部倍率 1×、保守 read preflight、同一 Turnip `68dafb89`。全程使用 debugbus 与 Litep，没有截图采样。

当前已安装 APK105（SHA-256 `0ee87602e286bfb8ebee1df9827bf10c3b9528e85bcfb0b20961e1a163c07b98`），默认 720p 手持、1×、FIFO、有界呈现交接。同一呈现实现在 APK104 的 60 秒验证为约 29.50 FPS，P90 36.738 ms；相比旧约 20 FPS 已明显改善，但仍有短暂降档及显示长帧。本轮没有进入存档场景。

APK105 已核对安装包 SHA、PID 32619、默认 FIFO/720p 手持/1× 及 `present_worker=1`。清空实验属性后的 6 秒启动复验为 180 个完整帧，平均 33.331 ms、P90 33.608 ms，181 个 interval 样本全部为 2；这项短复验只证明默认开关和新包路径一致，不能覆盖前述长测残留。结束时 ring、HLE、CPU detail、GPU detail 和 capture 均关闭，设备仍在首屏。

## 两个问题分别验证

原版适配将 Vulkan query 的绝对值乘 `timestampPeriod` 后直接作为 Guest GPU 时间。Odin3 实测一条报告为 GPU `524080843354953 ns`、CPU 单调时钟 `247611040147413 ns`，相差约 276470 秒。Guest 的系统 tick 来自 `CLOCK_MONOTONIC`，两者不能直接比较。

现在由同一 Vulkan 队列上的真实查询建立到 Guest 时钟的映射，八次有围栏的采样选最窄区间，记录完整不确定度区间；按相对 tick 换算并处理有效位回绕。定期在呈现边界排空旧报告后重新校准，也检查休眠时钟偏移变化。没有修改 Guest 时间流速。实际合成 GPU 测试验证 TOP/BOTTOM 报告的单调关系及其与 CPU 观测区间的一致性；仅恢复旧换算的负例报 `GPU reports use a different epoch from Guest GetSystemTick`，修复版通过。

**单独修正时钟没有解决首屏 20 FPS。** APK93 仍保持 interval=3、约 50.16 ms/帧。因此不能把这次帧率收益归因于时钟校准。Mac 的时钟修复注释讨论的是其 TOTK 路径，也不能直接当作 BOTW 具体降帧原因的证明。

第二个差异是阻塞位置。旧 Android 适配在 `QueuePresentTexture` 内完成上屏后继续 sleep，同时持有 NVN 前端锁。Mac 的 `hle_nvn.c` 则记录每张交换纹理的可复用时间，将节拍等待放在 `AcquireTexture`。Android 现在复用 Mac 的 `nvn_pace.h` 节拍算法，按窗口维护 pace 和逐图像 available 时间；Acquire 的等待释放前端锁，Present 内不再人为 sleep。GPU 复制、围栏与保守完成检查仍保留，没有将游戏的 interval 固定成 1 或 2，也没有加入 60 FPS / VFR 补丁。

## 首屏 Litep 结果

下表均为基础记录的完整成功 present 窗口，不混用全量 HLE 或 CPU detail 的耗时。CPU scope 统计为经过时间，不是 CPU 忙碌时间。

| 版本 | 完整帧 | 平均 / P50 / P90（ms） | interval 观测 | 结果 |
| --- | ---: | --- | --- | --- |
| APK92，增加诊断 | 119 | 50.273 / 49.892 / 62.989 | 120 个样本全部为 3 | 约 19.89 FPS |
| APK93，仅修正 GPU 时钟 | 119 | 50.164 / 50.728 / 61.208 | 仍为 3 | 帧率无实质改善 |
| APK94，Acquire 节拍 | 171 | 35.106 / 33.384 / 43.677 | 154 次为 2，18 次为 3 | 有提升，仍有短暂降档 |
| APK95，细分 Present 诊断 | 179 | 33.337 / 33.340 / 33.623 | 180 个样本全部为 2 | 此窗口约 30.00 FPS |
| APK97，持续 60 秒 | 1752 | 34.245 / 33.352 / 37.420 | 1657 次为 2，96 次为 3 | 约 29.20 FPS，仍有短暂降档 |

APK92 的 Present 人为等待为 25.833 ms，GuestCondition 为 14.604 ms，Draw 为 2.785 ms。APK95 的 Acquire 节拍等待为 18.303 ms，GuestCondition 降到 1.321 ms，Draw 为 2.357 ms；实际 `QueuePresentKHR` 平均 5.406 ms、Overlay 1.217 ms。完整帧从 50.273 ms 降到 33.337 ms，证明这次收益不只是把同样的等待从一个 scope 改名或移动。

这几份文件无丢块、无 CPU scope 配对错误，各保留一条捕获边界 GPU zone 不完整诊断。成功调用 present 的间隔不是屏幕光子时间；没有 scheduler/KGSL 证据，不据此宣称 GPU 利用率或 CPU 睡眠比例。GPU profiler 的校准上下文与给 Guest 的 NVN counter 映射是两套独立契约，不能互相替代验收。

APK97 的长采集否定了“6 秒保持 30 FPS 就代表首屏已稳定”的判断：P99 为 56.125 ms，136 帧超过 40 ms。首个降档窗口前，第 25–33 帧的 `Host.Present.QueuePresent` 多次达到 10–19 ms，而 GPU WaitIdle 仅约 0.02–0.05 ms；随后第 34 帧 Acquire 节拍等待达到 41.021 ms，完整帧为 59.380 ms。该窗口距时钟重校准还有数秒，不能归咎于重校准。捕获中的一次重校准实际耗时 1.923 ms。

APK98 单独改用 Mailbox（实际 5 张 WSI 图像）后，60 秒平均 33.722 ms，P90 33.824 ms，但仍有 42/1780 个 interval=3 样本。`QueuePresent` 平均 7.129 ms、最大 25.499 ms，不能认定问题消失，也不能把较短窗口的改善完全归因于模式变化。最终默认保留 FIFO；`debug.mng.present_mode=mailbox` 仅用于隔离实验。Mailbox 可能替换尚未显示的图像，因此成功 present 计数不能代替实际显示计数。

另一个 Mac 路径差异是 `mtl_link_wait_tick`：显示回调活跃时，Acquire 跟随显示节拍，独立时钟只是回退。Android 已有的 Choreographer 回调原先仅唤醒 VI 事件，没有接入 Acquire。现在把原始帧时间和实际刷新率送入底座，按 Guest interval 的 60 Hz 单位选择显示拍数；漏回调按真实时间推进，迟到的帧立即开始，回调停顿或无法整除时回退。等待期间释放 NVN 前端锁及 Guest 虚拟核心。60/90/120 Hz、重复/丢失回调、停顿和非整除后备的测试通过。但实机否定了直接将它作为默认：APK99 的 1727 帧平均 34.751 ms（约 28.78 FPS），147/1728 个 interval=3；APK100 的第二次 60 秒为 34.797 ms、152/1726 个 interval=3。Present 的部分等待转移到显示拍等待，完整帧没有改善。该路径恢复为 `debug.mng.display_pace=1` 才启用的实验，默认关闭。

同 APK99、FIFO 不变，关闭显示回调节拍后的 20 秒为 592 帧、平均 33.832 ms、P90 35.828 ms，17/593 个 interval=3。该反向对照与两次开启结果共同阻止了一个仅凭 Present scope 变短就会接受的回退。

## 有界呈现交接

Android 的 WSI 调用即使选 Mailbox 仍会等待。新增容量为一帧的 `SerialHandoff`：仅把 Present 交给工作线程，命令翻译、Guest 命令内存读取仍同步执行。调用方必须等工作线程实际取得原有引擎锁才返回，因此下一次引擎操作不能超车；后续 Acquire/Finish/纹理 Finalize 通过原有锁和 GPU 完成路径等待。没有绕过 Fence 或提前回收 GPU 副本。关闭时先完成 GPU 工作，再关闭记录器并 join 工作线程。

合成并发测试覆盖锁所有权交接、队列背压、工作顺序及关闭排空。首次测试发现 `std::function` 的 moved-from 状态不能当作空队列标志，出现重复执行；改成 `std::exchange(job_, {})` 后通过，失败版本没有安装到设备。真实运行按 `present_worker=1` 的 debugbus 身份和 `mng-present` 线程确认。APK104 同包开关对照：工作线程 60 秒为 1771 帧，平均/P50/P90/P99 为 33.899/33.351/36.738/54.491 ms，约 29.50 FPS；关闭工作线程 20 秒为 575 帧，34.793/33.368/41.241/57.269 ms，约 28.74 FPS。interval=3 分别为 60/1772（3.39%）和 50/576（8.68%）。两窗口长度不同，不能将它表述为所有场景的固定收益；APK105 把相同的交接路径设为 Android BOTW 默认，`debug.mng.present_worker=0` 可做回归对照。

工作线程版本的 1772 个样本中，新建 pipeline 和 shader load 均为零，该窗口未观测到应用层实时 pipeline 创建或 shader 加载，现有证据指向同步与节拍等待。完整帧由实际成功呈现线程界定，Guest/NVN 调用仍在原线程，不能只看呈现线程的 scope 或把未标注时间解释为空闲。

仍有边界：该窗口不是严格稳定 30 FPS，仍有 3.39% 的 interval=3；独立 SurfaceFlinger 短窗口的 126 个实际显示间隔平均 34.383 ms，中位数 33.325 ms，存在 50/66 ms 长显示间隔。它与 60 秒 PROF 不是一一对齐采集，不能用来精确计算丢帧比例。这里只关闭原先持续约 20 FPS 的首屏退化，少量长帧与显示节拍仍需后续优化，不宣称具体游戏场景已经完成性能验收。

## 证据与检查

本地证据目录为 `/Users/bytedance/workspace/native-switch-study/porting-20261024-lifetime`，包含原始 PROF、SDK sidecar、PID/SHA 身份、原生日志、APK 和预算 JSON。

| 文件目录 | 基础 PROF SHA-256 |
| --- | --- |
| `candidate92-title/basic` | `d1cd6a21a6b3cc54b38eda112925cd1addc998710ad1cd0a9c3acd4a6d759514` |
| `candidate93-clock-title/basic` | `38d0ab001cddd72223d2a73e6a2072c3f9e5e76262d5468b4bddd376e7274c7f` |
| `candidate94-title/basic` | `568a7174e69f50a4a75cd687f73ed3ade093022de70b75ada058c6c7a2361b3f` |
| `candidate95-title/basic` | `483ef446e32b4ccdb1ae0bc3b5efc4c44b6e9a461a5a3dda170bb29dee6c9e83` |
| `candidate97-title-60s/basic` | `fed06163a412ddcf0288715495da2259081b51707983aa4a068aead4172fe706` |
| `candidate98-mailbox-title-60s/basic` | `6621cf5f5d059dab02f614904fe442672b4e8c28dcc0f793573c09d27dfe54cd` |
| `candidate99-display-fifo-title-60s/basic` | `474e723ef4b593662e333d8483eb3161b0c11d4b35834a76e002a8eb8a003c74` |
| `candidate99-no-display-fifo-title-20s/basic` | `5473bd8acfc81aa39ab752c6ef4633d2cc1ce9cd247f92c04dab2e4112f6e1ec` |
| `candidate100-title-60s/basic` | `cbe2eac688a934ebd98bd1d7b2c8342265539b2fdc38c5b723508fe5f80e74c8` |
| `candidate104-present-worker-title-60s/basic` | `ab0e65554e5f478bcb91817f012669021f47e92d173e7a5c5626bdc5cfa4177e` |
| `candidate104-sync-title-20s/basic` | `65ed66c09b6026d5c6260a45b97c69a4dc4d94315775303d17f1920f6b3b6b5d` |
| `candidate105-default-title/basic` | `61c8a3e1bdb90f51a731cfeb7f002226bd12dc708853cd335fae6c07aa53ee06` |

已通过宿主 GPU 时钟变换测试（相对时间、32/48/64 位回绕、溢出安全）、2/3/4 图像节拍模拟、实际 Turnip 的 NVN 时间戳与既有像素/生命周期回归、debugbus/HLE 汇编 ABI 测试。负例与修复版日志分别为 `probe94-negative.log`、`probe94-pass.log`。这些结果不代替场景内渲染正确性或性能验证。

APK94 首次构建缺少 profiler 声明，失败后曾误重装上一版 APK93；其 SHA 与 APK93 完全一致，已明确归档为 `botw94-build-failed-stale93.apk`，没有纳入 APK94 的性能数据。正式 APK94 来自随后成功的 `apk-build94b.log`。

## APK106：接入 MacNative 的 60 FPS 配套补丁

在 APK105 的首屏 30 FPS 改善之后，按新要求加入逐游戏计时补丁，而非仅修改呈现上限。实现和身份验证见 `games/botw/guest_patches/REALTIME.md`。APK106 SHA-256 为 `1d4573c51463c03a6295e38775079023a221b43c50fb05fbed8ce3dde3e75826`，Odin3 PID 17625，720p 手持、1×、原 Turnip、present worker 开启。首个 30 秒实机计时日志记录 1782 帧、有效真实时间 30.019417 s、游戏时间 30.006673 s，比值 0.999575；此处不是显示器刷新统计，也不是场景战斗验证。
