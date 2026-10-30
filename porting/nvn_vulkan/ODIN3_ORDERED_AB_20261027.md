# Odin3 有序翻译实机验证：未通过性能验收

日期：2026-10-27。

## 结论

**当前实现没有改善真实 BOTW 的性能，不能默认启用。** 同一 APK、相同现有存档的四次重启对照中，串行加权 **21.154 FPS**，有序加权 **19.444 FPS**，观测差异 **−8.081%**。

更关键的是：两个有序窗口的 `ordered_submits` 都是 **0**，测量相邻状态快照分别增加 **890 / 906 次回退**，全部为原因 4（未支持 opcode）。启动至窗口结束的累计回退分别是 **2408 / 2342**。这是“开关生效但整批提交全部同步回退”，不是后台 Translate 正常运行后收益偏小。

因此，[本地验证报告](ORDERED_TRANSLATION_20261027.md)中的 **384/400 合成提交进入异步路径**不能推广到这个 BOTW 场景。本轮实际长 Translate 仍处于前端全局锁内；注册、释放、Present 等有序消息和相关等待则已经启用。

四次运行没有发现崩溃、设备丢失、未捕获输入的 fatal 指标或明显新增黑屏/缺绘。此结论仅覆盖本轮固定场景和截图，不是全游戏正确性或输入延迟验收。

## 设备、版本及数据边界

- 设备：AYN Odin3，序列号 `6edb0593`，device/product `sun`，Android API 35，arm64-v8a。
- ROM fingerprint：`qti/sun/sun:15/AQ3A.250728.001/eng.Odin3.20260616.191954:user/dev-keys`。
- 另一台 Thor 虽在线，但所有设备命令显式选择 Odin3，没有对其执行操作。
- 包：`org.macnative.android.botw`。
- BOTW Switch 1.6.0，main Build ID `8E9978D50BDD20B4C8395A106C27FFDE`。
- APK SHA-256：`21151c0fd10850ed5f567364b4263bd30e2c1f890d3621b8ec84024a46dc6286`。
- 原安装 APK SHA-256：`8656c2deb000cc88ee8f6c931b3421e14894ae5981158dbf0cf83831ef425d26`，替换前已拉取并校验。
- Turnip SHA-256：`e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`。
- 沿用 720p handheld、internal scale 1×、已有 realtime/60 FPS patch 配置及 shader/PSO 缓存，不重新导入游戏内容。

用户本轮明确要求实机验证。执行前使用设备上下文接口核验身份，随后备份旧 APK、偏好、存档、账号、overlay 和缓存，使用 `adb install -r` 原位更新。没有卸载、清数据、Root、锁频、删缓存或部署其他存档。

安装前后 **78 个被审计文件的 SHA-256 全部相同**，Android 偏好字节相同。

## 对照方法

开关是启动时选项，不能热切换，因此四组分别启动独立游戏进程，顺序为 **OFF A → ON A → ON B → OFF B**。采用两端串行、中间有序的顺序观察重复漂移；不是同一进程的热切换实验。

每次通过已有 autoloader 打开同一现有存档。要求连续 20 秒同时满足：

- 已打开存档、成功 native present 持续前进；
- 每次状态采样至少 1000 draws，并有 dispatch；
- 没有新 shader load 或 pipeline creation；
- GameActivity 在前台，状态样本足够新鲜。

之后采样约 45 秒，以状态样本 age 修正时间。度量为 **成功 native Vulkan present 次数/秒**，不是屏幕合成时间戳或经独立证明的唯一 Guest 帧。

FPS 窗口内 profiler recording、详细 CPU tracing、preparation validation 和 perf sampling 均关闭。OFF A/ON A 各额外采一个独立 3 秒 PROF，均在 FPS 窗口之后。截图、温度查询也在计数窗口之外。

非 ordered 的已列举渲染控制在四组间相同；准备缓存始终开启。游戏和 Host 缓存保留，不是冷缓存比较。没有注入移动或相机输入；截图可确认神庙内视角一致，但没有精确坐标/相机仪表，也无法排除物理用户输入。

## FPS 与工作量

| 测试顺序 | 模式 | PID | 成功 presents | 修正后区间 | FPS | draws 中位数 | dispatch 中位数 |
|---|---|---:|---:|---:|---:|---:|---:|
| OFF A | 串行 | 20864 | 969 | 45.728 s | 21.190 | 3652 | 14 |
| ON A | 有序 | 22920 | 881 | 45.633 s | 19.306 | 3645.5 | 14 |
| ON B | 有序 | 25109 | 894 | 45.654 s | 19.582 | 3592 | 14 |
| OFF B | 串行 | 26961 | 966 | 45.746 s | 21.117 | 3649 | 14 |

- 按测量时长加权：OFF **21.153554**，ON **19.444132**。
- 相对差异 **−8.081019%**；两次 ON 均低于两次 OFF。
- OFF 重复值接近，ON 的第二次略快。站立场景仍有动画，工作量不是逐字节相同。
- 未锁频，Android 调度、频率、温度及每次重启都可能带来差异。不能把 −8.081% 当成精确通用回归幅度或严格因果效应。
- 最可靠的验收结论是：**没有性能收益，目标异步 Translate 路径没有在该场景执行。**

Thermal HAL 的总体状态为 0，但缓存 CPU 温度与 fresh HAL 数值差异很大，并有 `ibat` status 6。没有将这些读数或截图叠层的 CPU/GPU 百分比当成“无降频”“GPU 空闲”或进程独占利用率证明。

## 异步覆盖率与 trace

| 有序组 | 窗口相邻异步提交增量 | 回退增量 | 原因 4 增量 | Capture 字节增量 | Drain 调用增量 |
|---|---:|---:|---:|---:|---:|
| ON A | 0 | 890 | 890 | 0 | 91575 |
| ON B | 0 | 906 | 906 | 0 | 93334 |

这些增量来自 cadence 前后相邻状态快照，边界不是逐条事件与 FPS 样本严格对齐，因此不把回退次数直接等同于 presents。

`ordered_admitted` 会包含注册/释放等操作；即使请求数很多，也不能说明 Draw 已后台翻译。原因 4 来自 `ordered_submission_engine.inc` 的未支持 opcode 分支；回退在 `nvnQueueSubmitCommands` 中继续调用原 `xbg_submit`。当前二进制只记录原因类别，没有记录第一个具体 opcode，所以**尚不能确定是哪一种命令首先阻断整批异步化**。

两个 PROF 均按完整的 60 个 source-frame 区间分析。实际 Translate 线程是 OFF A 的 TID 21094、ON A 的 TID 23137；header 中 main-thread 则是 presenter，不能混同。

| 同一 Translate 线程上的 scope | OFF A 平均 ms/source frame | ON A 平均 ms/source frame |
|---|---:|---:|
| `NVN.Translate` inclusive | 28.366 | 27.455 |
| `NVN.FrontLock.Hold` inclusive | 28.468 | 28.152 |
| `NVN.FrontLock.Wait` | 0.013 | 1.122 |
| `nvnQueuePresentTexture` inclusive | 0.056 | 0.688 |
| `NVN.Ordered.Capture` | 未执行 | 0.006 |

全索引范围未观察到 `NVN.Ordered.Execute`，与运行时异步提交计数为零一致。ON A 其他线程也观察到有序 drain 和前端锁等待。

这些是 **elapsed scope**，不是 on-CPU 时间；父子 scope 重叠，不能相加，线程间也不能加成帧时长。约 3 秒的独立 profiler 窗口不是上面 45 秒的无插桩测量，不能用其时长差异直接解释全部 FPS 差额。没有 scheduler/KGSL sidecar，也没有建立 CPU/GPU 校准时间轴或完整关键路径。

### 采集完整性

| 模式 | PROF SHA-256 | 事件数 | accepted / skipped chunks | 完整 source frames |
|---|---|---:|---:|---:|
| OFF A | `e19899484637e6ae85efe8254dce4bb64873f79cf54508e07bcaebcde840b515` | 3244594 | 290 / 0 | 60 |
| ON A | `62d9f04441fe3164ea50cefc882f7a2e653e24049ce49b54560e0f12c06d4a54` | 3223201 | 288 / 0 | 60 |

两者容器完整、CPU `truncated=false`、dropped-chunk markers 为 0，均验证了设备/本地文件哈希和 PID/capture ID。仍有采集边界未闭合 CPU spans、一个不完整 GPU zone，以及 GPU orphan times（OFF 2、ON 12）；不能宣称所有端点无损，也不据此做精确 GPU 空闲/忙碌结论。

## 画面与结束状态

四张截图均已人工视觉检查：同一神庙内构图，角色 idle 动画姿态不同，未见明显新增黑屏、缺绘或纹理破坏。这不是逐像素一致性或全场景覆盖，未测输入延迟。

测试开始前没有游戏进程；结束时停止本轮拥有的最后一个测试进程 26961。临时 `debug.mng.ordered_translation` 和 `debug.mng.autoload` 都恢复为原来的空值，后续正常启动继续使用默认串行模式。保留新 APK，不回滚用户数据。

结束审计：78 个文件均在，无新增/删除；**77 个哈希相同，仅 `option.sav` 的 4 个字节变化**，长度仍为 344。字段含义未验证，不把它描述为“只是计时”或“设置完全没变”。未进入选项菜单，没有覆盖原存档；安装前归档及运行后字节均保留供恢复，但本轮没有自行恢复。

## 后续修正方向

继续保持默认关闭。下一轮应先增加**具体回退 opcode、位置和频率**的有界统计，在真实 BOTW 上明确阻断点，再为对应命令补所有权/顺序支持，或仅将证明安全的区段交给 worker。

验收先要求真实场景的主 Translate 确实离开 `front_mutex`，再讨论帧率。不能简单把未知指令加入允许列表，不能删 hazard/fence，也不应继续用合成测试 96% 的覆盖率说明游戏已有收益。

本轮只验证交付的同一 APK，没有再修改 renderer 或以未验证新包替换测试对象。

## 私有证据

根目录：`/Users/bytedance/workspace/native-switch-study/android-translate-20261026/odin3-ordered-20261027/`。

- `deployment.json`、`device-identity.json`：设备和安装验证。
- `before-installed.apk`、`before-save-settings-cache.tar`：原包和数据备份。
- `preserved-files-*.json`、`preservation-final.json`：安装前后及运行后的保留审计。
- `off_a/on_a/on_b/off_b/result.json`、`scene/events.jsonl`、`screen.png`、`native-full.log`：四组证据。
- `cadence-summary.json`、`trace-scope-summary.json`：对照和只读索引归约。
- `off_a/litep/`、`on_a/litep/`：原始 PROF、producer sidecar、精确身份 sidecar。
- `off_a/index/d97e45a0c205486482dd749d2828ab8d.litep.sqlite`、`on_a/index/d0ecadbee7274651bdd7b66993866928.litep.sqlite`：可恢复分析的索引。

存档、账号、原始游戏数据和私有备份不加入 Git、不上传。未提交或推送代码。
