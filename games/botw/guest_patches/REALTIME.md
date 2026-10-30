# BOTW 1.6.0 Android 60 FPS 补丁

此实现移植自本仓库 MacNative 的 `game_patches_botw.c`，只接受 Switch BOTW 1.6.0：main Build ID `8e9978d50bdd20b4c8395a106c27ffde`。不适用于 1.9.0、TOTK 或 Wii U。

## 组成与边界

Android 默认启用 VFR、淡入淡出、UI、六次物理子步、手柄连发和暂停菜单计数修正。每次 Guest `QueuePresentTexture` 开始时发布下一帧的实测时间；不等到宿主异步呈现线程才更新。时间取最近五个有效帧间隔的平均，平均上限 1/15 秒。超过 250 ms 的暂停不做追帧，恢复时重置采样。低于 15 FPS 的持续卡顿仍会使游戏时间落后，这是沿用 Mac 的稳定性上限，不能称为任意帧率下等速。

普通画面有效呈现间隔为 1（60 FPS 上限）。Guest 自己请求的间隔保留为诊断值。影片实例非空时使用间隔 2（30 FPS），原来的音频 48 kHz 时钟不变。此上限不保证复杂场景达到 60 FPS。

补丁数据页由模块加载器在重写镜像之后单独分配 16 KiB，权限 RW、不可执行；代码仍是 RX。页面偏移固定为 `0x2e20000`，ADRP 相对距离不依赖装载基址。回调所有者在解除模块映射前销毁。所有代码修改在私有 RW staging、重定位和 seal 之前完成，无运行时代码写入。

调试对照可在启动前设置 `debug.mng.realtime=0`，恢复原版计时和游戏请求的呈现间隔；设为 `1` 或清空属性恢复默认。不要单独关闭 UI/连发修正却保留 60 FPS。

## 身份与验证

输入是经过独占访存 acquire 修正的 Android 镜像（SHA-256 `60474b0e3056daf4aee06c875de30bb001310d934437da0c4e99bdb76ca05007`），或已加 1080p 补丁的镜像（`d48e4b5eb443a5a9f8f370101d4fd7182a36830a4b87efa5a6c2be9edff025ea`）。执行器同时核验原始 NSO SHA、Build ID、平台、完整输入镜像和每个修改点；任何失配均拒绝，不能部分应用。

共 60 个指令位置。720p 输出 SHA 为 `3663d67e1f02a58450c3ac18751d0644d2288086432f7590ed6dc669fc963393`；1080p 输出为 `72c5fd02a188ef150f7fdc1e1b8b224d08bcdb4874a09241c4f96643f75ca3d9`。原始、改写后游戏镜像均留在 Git 外。

影片状态地址并非直接照搬：在当前私有镜像反汇编确认 `main+0x1600e70/74` 向 `main+0x2cbef40` 保存实例，`main+0x1a4d6ec/f0` 清零同一地址，初始值为零。这四条指令也进入启动校验。

`mng_botw_patch_probe` 检查错误版本、错误页面、损坏输入、重复应用的原子拒绝，以及两个分辨率路径、时钟抖动/暂停和影片间隔切换。`verify_android_realtime.py` 比对 Android 实际写出的全部指令，并用 Unicorn 在相同 1.6.0 输入上执行 VFR、淡入淡出、控制器、UI 和物理子步的正反例。Mac 旧验证器的 UI/物理部分只覆盖 1.9.0，因此这里补了 Android 1.6.0 的实际输出执行检查。模拟执行不代替实机动画、战斗和影片音画同步验收。

## 诊断

Litep 保留 `Native.RequestedPresentInterval`、`Native.PresentInterval` 和 `Native.GameStepUs`。前者是游戏原请求，后两者分别是有效呈现间隔和发布的游戏步长。日志每 30 秒报告有效样本 real_s/game_s 比值；加载暂停被排除，不能把这个比例当成整个启动过程的时间比。
