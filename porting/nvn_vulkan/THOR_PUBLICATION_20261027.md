# Thor 发布解耦验证：基线 GPU 时钟检查失败

日期：2026-10-27。分支：`codex/android-zelda`。

## 结论

**未通过，停止在安装前的 GPU probe。** 用户已明确把设备从 Odin3 改为 Thor，
因此本轮使用 Thor `9c2841a4`。`publication_queue=0`、`ordered_translation=0`
的基线触发 `GPU reports use a different epoch from Guest GetSystemTick`，probe 返回 1。
新 APK 未安装，未启动游戏，未运行 ON / ordered probe、ABAB 或 Litep；没有 Thor 的新帧率结果。

这不是发布队列开启后的回归证据。Odin3 的 21.129 → 27.993 FPS 仅属于
[Odin3 W1 报告](ODIN3_PUBLICATION_W1_20261027.md)，不能移作 Thor 数据。
D1 没有产生 Thor 新结论，W2/W3 未实施；Odin3 W1 尚未满足的完整验收项也没有因此关闭。

[任务书 §4](SPEC_PUBLICATION_DECOUPLING_20261027.md)规定：
“出现 fatal signal、device lost、`bad_weak_ptr`、断言失败、明显缺绘或黑屏：立即关闭新开关，保留现场（日志、精确 ELF 符号化的栈），写报告停止。”
本次是自测 `need()` 正确性断言失败并被捕获，未发生未捕获异常或 fatal signal。
开关从一开始即为 0；未绕过检查、放宽误差门槛或继续性能实验。

## 本轮修改

- `android/tools/benchmark_publication.py` 的显式设备表增加 Thor `9c2841a4` / `AYN Thor`，保留 Odin3。
  仍校验所选序列号对应型号、预期 PID、APK SHA-256、BOTW main Build ID 和前台活动。
- 追加本报告及三份验证文档。未改渲染器、时钟实现或默认开关。
- Mac/Android Vulkan 继续共用实现，没有新增 Thor 独立分支。未提交、未推送。

## 身份

| 项目 | 本轮证据 |
|---|---|
| 设备 | AYN Thor，`9c2841a4`，`kalama`，Android 13 / SDK 33 |
| 系统指纹 | `qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys` |
| GPU | `Turnip Adreno (TM) 740`，vendor `0x5143`，device `0x43050a01` |
| 驱动 | Mesa 26.3.0-devel，git `68dafb89a2`，Vulkan 1.4.363 |
| 驱动 SHA-256 | `e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d` |
| 驱动 ELF Build ID | `ff30c504023c80a8a8066d092fab72e41eb9ce36` |
| probe SHA-256 | `dd5de69498534b24be43d95ee2c3f7679ed2726cacab1ebc29d6def5d4068ea2` |
| probe ELF Build ID | `9b4942bc17cbdbf6238a0cd43d8fdd9f798cb3d5` |
| platform SO SHA-256 | `d509eb952dbab77eb071b069ec1845d3f7fe346b278fe6d6c404d4e74495e887` |
| platform SO ELF Build ID | `63bd451b7367f5f89c3bea522c64f433b5e8b8ca` |
| 设备现有 APK SHA-256 | `8656c2deb000cc88ee8f6c931b3421e14894ae5981158dbf0cf83831ef425d26` |
| 待验证 APK SHA-256 | `15e75fcbdf7c80e0affa8377a24bdfcde21870c6d53ab3c00474c7655d739da9`，未安装 |
| 包名 | `org.macnative.android.botw` |
| 游戏 PID / live main Build ID | 无游戏进程；本轮没有加载 Guest 模块，因此未取得 live main Build ID |

probe 使用 W1 第二轮已重链产物，上传文件逐一校验 SHA。执行路径为
`/data/local/tmp/mng-thor-publication-20261027/probe`；诊断驱动与同目录动态库加载，
没有改动应用内驱动。实际启动日志确认 Turnip / Adreno 740。

预期后续游戏验证仍限定 BOTW 1.6.0 main Build ID
`8e9978d50bdd20b4c8395a106c27ffde`；该值是准入要求，不是本轮已经验证的运行身份。

## 失败证据与定位

初始化校准日志：

```text
mode=submit_wait_midpoint gpu=139028 host_ns=99857811117346
deviation_ns=236406 period=52.0833321 bits=48
```

首组 report 转换日志：

| raw GPU tick | 换算后 ns | 转换时观察到的 host ns | host − mapped |
|---:|---:|---:|---:|
| 150194 | 99857811698908 | 99858841720288 | 1030.021380 ms |
| 150210 | 99857811699741 | 99858841884820 | 1030.185079 ms |

从选中的校准锚点到第一条 report，GPU raw 增加 11166 tick，按报告的周期换算约
0.581562 ms；对应宿主观察时间增加约 1030.602942 ms。换算时刻晚于 GPU 执行时刻，
上述差值不是 GPU 执行耗时。自测实际 `before/after` 边界未输出，不能把表中差值
直接称为断言边界误差，更不能据此断言硬件计数器在空闲期间暂停。

源码路径：

1. `porting/nvn_vulkan/guest_clock_engine.inc::calibrate()` 在空闲队列上做八次
   fenced timestamp，选择最窄的 host submit/wait 区间，中点与 raw tick 建立锚点。
2. `porting/runtime/gpu_clock.h` 使用 raw 差值和 `timestampPeriod` 映射到宿主单调时钟，
   再转换为 Guest tick。单测验证了公式、回绕和范围，没有验证真实 GPU 计时源连续性。
3. `guest_clock_engine.inc::refresh_needed()` 只在 present 边界检查刷新；当前条件为
   锚点寿命或 BOOTTIME 与 MONOTONIC 差值变化，未检测 raw counter 与宿主推进不一致。
4. `porting/nvn_vulkan/vulkan_self_test.cpp:546` 检查 TOP 不早于提交前 10 ms、BOTTOM
   不晚于完成后 10 ms，在首组 report 失败。下一条单调顺序断言尚未执行。

当前可判定的是：**该运行中，现有 GPU query → Guest 时钟映射未满足正确性要求。**
尚未区分计时源连续性、查询结果来源/生命周期和校准锚点适用性，不能直接归因于
驱动、电源管理或发布队列。百万次纹理循环和前端锁测试已执行，但它们不代替完整 GPU probe。

精确 ELF 中 `mng_nvn_self_test` 的符号地址为 `0x1688c0`，这是函数入口而非故障 PC。
自测通过 `need()` 抛出 `runtime_error`，由函数尾部 catch 打印 FAIL、清理并返回 1。
没有原生 fault PC 或现场 unwind；保留了精确 ELF、Build ID、符号表和源文件 SHA，
不把源码定位伪装为符号化故障栈。系统 crash buffer 中旧的 SurfaceFlinger/Google 记录
不属于本轮 probe。

## 检查结果与未执行项

| 检查 | 结果 |
|---|---|
| probe / SO / 驱动上传后 SHA 校验 | 通过 |
| Thor 基线 probe，publication=0 / ordered=0 | 失败，exit 1，时间戳正确性断言 |
| Thor publication=1、ordered probe | 未运行，基线失败即停止 |
| 现有主机 `mng_gpu_clock_probe` | 1/1 通过，仅算术覆盖 |
| benchmark 脚本语法与显式设备映射 | 通过，无设备命令执行 |
| APK 安装、自动读档、ABAB、Litep | 未运行 |
| Mac probe / 全量构建 / lint | 本轮未重跑；没有渲染器源码变化，历史结果见 Odin3 W1 报告 |

因此没有 Thor ABAB 原始表、等待转移表、队列/drain/索引计数或 PROF 完整性结果。
GPU 与 CPU 的跨时钟关联未验证，不能进行跨时钟阶段耗时归因。

## 数据保全与最终状态

- 应用未安装、未启动、未清除或卸载，未执行存档导入。现有 APK 哈希保持一致。
- 只在 `/data/local/tmp/mng-thor-publication-20261027` 写入诊断产物与 probe 私有缓存。
- 启动属性前后完全一致，没有开启应用采样；probe 已退出，无遗留 probe 或游戏进程。
- 未 root、锁频或替换应用驱动。未操作已断开的 Odin3。
- 原计划的安装前后 78 文件哈希流程没有执行，因为安装阶段未开始；不能把“未写应用数据”
  表述为“78 文件全量哈希复核通过”。保留设备目录、属性、包信息和最终状态记录。

## 下一步边界

继续性能优化前，先独立处理 GPU report 时钟的正确性：

1. 用同一 driver / ELF 身份做有界的 query 校准验证，记录每次 query 的编号、reset、写入、
   availability、提交/完成 host 边界和 raw 值；分别安排紧邻提交与 CPU 空闲间隔，区分
   计数器推进问题与旧 query 值。该验证是下一轮工作，本轮没有继续重跑或调参。
2. 若证据证明锚点失效，需要在共享 Vulkan 路径内解决锚点有效期和 report 所属代次。
   仍在途或尚未读回的 report 不得套用另一个时间段的新锚点。
3. 保持 Guest 单调时钟、TOP/BOTTOM 顺序和 report 写回可见性；不能统一替换成 CPU 读回时间，
   不能放宽 10 ms 检查以获取性能数据，也不增加 Thor 专有 Vulkan 实现。
4. 正确性 probe 全部通过后，再运行自动读档、Thor 同进程 ABAB 和独立 Litep。
   继续保持 publication 默认 0；W1 的 identity 退休与生命周期验收缺口须另行闭合，
   再根据实测锁热点推进 Invalidate 等后续工作。

## 证据位置

仓库外证据根目录：
`/Users/bytedance/workspace/native-switch-study/android-translate-20261026/thor-publication-20261027/`。

- `probe-off.log`：完整失败输出。
- `probe-identities.json`、`symbol-provenance.json`、`*-elf-notes.txt`、`probe-symbols.txt`：产物身份与符号。
- `clock-deltas.json`：原始数值、派生差值及解释边界。
- `before-identity.json`、`before-properties.txt`、`final-properties.txt`、`result.json`：设备和最终状态。
- `script-validation.json`、`gpu-clock-unit-test.log`：本轮离线检查。
- `before/`：本阶段四个已有文件的起始快照。

本阶段差异：`android/build/thor-publication-20261027/change.diff`。
该文件仅记录本轮脚本和文档改动，不包含前几轮渲染器修改或私有测试资产。
