# 后续优化 Spec v2：打断"翻译持锁 → 生产者阻塞 → 翻译等消息"的串行环

> 2026-10-28：W0–W2 已完成。后续阶段见 [SPEC_PIPELINE_PARALLELISM_20261028.md](SPEC_PIPELINE_PARALLELISM_20261028.md)（v3）；本文的 §3 约束、§6 测量规范、§7 报告模板继续有效。

日期：2026-10-27。分支：`codex/android-zelda`。
依据：[ODIN3_IMAGE_FLUSH_20261027.md](ODIN3_IMAGE_FLUSH_20261027.md)、
[ARCHITECTURE_REVIEW_20261027.md](ARCHITECTURE_REVIEW_20261027.md)、
[ODIN3_COUNTER_REPORT_20261027.md](ODIN3_COUNTER_REPORT_20261027.md)、
[ODIN3_LIVE_FLUSH_20261027.md](ODIN3_LIVE_FLUSH_20261027.md)、
[ODIN3_GUEST_TAGS_20261027.md](ODIN3_GUEST_TAGS_20261027.md)。

这是给实施者的任务书。按阶段顺序执行，每个阶段都有**量化验收**和**停止条件**；
没有整帧证据的改动一律默认关闭。遇到停止条件就写报告交回，不要自行扩大范围或反复调参。

---

## 1. 目标与核心假说

**场景基线**：Odin3（`6edb0593`）/ Adreno 830 / Turnip `e61a373f…` / 720p 手持 / 内部 1× /
BOTW 1.6.0（main Build ID `8e9978d50bdd20b4c8395a106c27ffde`）/ 复苏神庙存档，
约 **20.9–21.3 FPS（约 47–48 ms/帧）**。

**假说 H1（本 spec 的主线）**：低帧率主要来自一个跨线程串行环，而不是单个慢函数。

1. Guest 渲染线程 R 调用 `nvnQueueSubmitCommands`，在**持有 `front_mutex` 和引擎 `mutex`** 的情况下同步翻译约 **28 ms/帧**。
2. 给 R 发消息的生产者线程 P 和 Guest 主线程 M 在翻译期间调用 `nvnTextureInitialize`、Flush 等 API，被 `front_mutex` 挡住，直到翻译结束。
3. R 翻译完后在 `nn::os::ReceiveMessageQueue` 上等 P 的下一条消息，约 **20 ms/帧**。
4. 于是帧时间 ≈ 翻译时间 + 生产者在锁释放后才能做完的工作，两段**串行**而不是**重叠**。

已有证据：

- counter report 运行中，R 的 56 次消息等待全部由同一线程 P（TID 10565）选中通知；P 在一个 300 ms 区间内的 80.302 ms 锁等待里，79.990 ms 与 R 的 Translate 重叠。
- image-flush 运行中，放行 Flush 后 M 的锁等待从 0.44 ms 涨到 486 ms（3 s 窗口），其中 `nvnTextureInitialize` 占 461.87 ms / 34 次。按 60 个源帧折算约 **7.7 ms/帧**。等待是转移了，不是消失了。

**上限估算**（不是目标，是判断收益空间用的）：如果串行环完全打断，帧时间下限约等于翻译时间 28 ms，即约 **35 FPS**。
之后能否继续提升，取决于翻译本身降本（W3）或真正异步（W5）。

**证伪条件**（W1 之后必须检查）：如果生产者的锁等待消失了，但 R 的 `ReceiveMessageQueue` 等待和帧时间都没有下降，就说明生产者被别的东西卡住（例如 `WorkerMgr::sync`），H1 不成立。此时转 W4，**不要**继续做 W2 的去锁。

---

## 2. 已从源码核实的事实（实施者无需重复确认，但不得违背）

| # | 事实 | 位置 |
|---|---|---|
| F1 | 串行模式下 Submit 以 `LOCK` 持有 `front_mutex` 直到 `xbg_submit` 返回 | `porting/nvn_vulkan/nvn_vulkan.cpp:1736` 起 |
| F2 | 引擎 `xbg_submit_now` 在整个翻译期间持有引擎 `mutex`（`MngEngineLock`） | 构建副本 `android/build/nvn-preparation/adapted/xbg_game.cpp:4050` |
| F3 | `xbg_legacy_tex_release_now`、`xbg_register_texture_now`、`xbg_register_sampler_now` 都拿同一把引擎 `mutex` | 同上 `:4022–4024` |
| F4 | Android 上 `gfx_async` 恒为 false（`:3937` 强制赋值；`prepare_engine.py:200` 断言原生 Present 要求它为假），所以上述引擎调用全部同步执行 | — |
| F5 | **结论：只把 API 从 `front_mutex` 中拿出来、仍然直接调引擎，等待只会转移到引擎 `mutex`。必须改成排队，延迟到下一次持锁时执行。** | F2+F3+F4 |
| F6 | 纹理 builder 按值存放在 Guest 的 `NVNtextureBuilder` 存储里，没有共享表 | `nvn_vulkan.cpp:381–389` |
| F7 | 录制命令时按 serial 对纹理做不可变快照，有独立的 `texture_snapshot_mutex`；翻译读快照，不读 Guest 的 `NVNtexture` 存储 | `nvn_vulkan.cpp:390–408`（`snapshot()`） |
| F8 | `nvnTextureInitialize` 用 `PUBLISH_LOCK`。锁内只有一次引擎调用：槽位里已有旧 identity 时调 `release_texture(previous)`。其余操作只读写 Guest 存储，或者本身就是线程安全的（`ConservativeRanges` 自带互斥，`nvn_new_serial` 是原子自增） | `nvn_vulkan.cpp:1021–1059` |
| F9 | `texture_initializations`、`texture_finalizations` 是普通 `uint64_t`，目前靠 `front_mutex` 保护 | `nvn_vulkan.cpp:362` |
| F10 | `nvnTextureCompare` 用的是 `LOCK`（会 drain 有序队列），但只读 Guest 存储 | `nvn_vulkan.cpp:1132` |
| F11 | `nvnTexturePoolRegisterTexture` 和 `nvnSamplerPoolRegisterSampler*` 用 `PUBLISH_LOCK`，串行模式下直接调引擎 register | `nvn_vulkan.cpp:1608–1635` |
| F12 | 已有锁序：front → engine；渲染/呈现 worker 从不拿 `front_mutex` | `nvn_vulkan.cpp:107–108, 113–116` |

---

## 3. 硬约束（违反即回退）

**正确性**
- Flush/Invalidate 不得变成无条件 no-op。GPU→CPU 写回、counter report、event、物理/虚拟别名都保留原保守路径。
- 不得删除 fence 延迟销毁、命令快照、按值纹理副本语义和同地址复用校验。不得跳过 Draw，不得吞掉断言或失败。
- 新行为通过 `runtime_policy.h` 统一默认值，并提供可热切换的 debugbus 命令（参考 `renderer_flush_image`）。Mac Metal、Mac Vulkan、Android 共用同一份实现。**新开关默认 0**，验收通过后单独提交改默认值。
- **有序翻译模式（`translation_owner` 非空）不启用新路径**，保持现状。本 spec 只针对默认的串行模式，这样风险最小。

**源码边界**
- 可改：`porting/nvn_vulkan/nvn_vulkan.cpp`、`porting/runtime/*`、`porting/tests/*`、`porting/nvn_vulkan/{prepare_engine.py,*_engine.inc,runtime_policy.h,vulkan_probe.cpp,vulkan_self_test.cpp}`、`android/tools/*`。
- 不得直接编辑 `android/build/**` 下的任何文件（都是生成物）。引擎改动只能通过 `prepare_engine.py` 的固定替换点，替换点失配时必须报错。
- 不得修改 `references/**`、Citron、Foundation、D2R 的只读检出。
- 新增 Guest 标签必须用 `mcp__ida__*`（`ida_workspace_open` 显式带模块基址）在精确 Build ID 上验证，固定指令字、入口指纹和 ABI。不得从其他游戏或版本移植地址。

**工作区卫生**
- 工作区已有约 240 个未提交修改，属于前几轮。**禁止** `git stash`、`git checkout -- .`、`git reset`、`git add -A`、`git clean`。
- 每个阶段把**只含本阶段改动的文件**的 diff 存到 `android/build/<阶段目录>/change.diff`。除非用户另行指示，不提交、不推送。

**设备与数据**
- 只操作 Odin3 `6edb0593`。每次操作前核对 ADB 序列号、包名 `org.macnative.android.botw`、`:game` PID、APK SHA-256 和 main Build ID。
- 安装前后比对 78 个设置/缓存/存档文件的哈希。**禁止**清除应用数据、卸载、使用 `connectedAndroidTest` 或 UTP（它会自动卸载并丢失验证数据，之前发生过）。instrumentation 只能直接运行。
- 不 root、不锁频、不换驱动。结束时恢复启动属性并关闭所有采样。`option.sav` 被游戏正常改写时不回滚，但要如实记录。
- 不提交游戏模块、RomFS、存档、密钥或原始 PROF。证据放在仓库外 `native-switch-study/android-translate-20261026/<阶段目录>/`。

---

## 4. 阶段总览

```
W0 诊断（必须先做，改动小）
 └─ W1a 无引擎依赖的去锁（小，安全）
     └─ W1b 发布队列：Initialize/Finalize/Register 移出 front_mutex（主线）
         └─ 决策点 D1：按 H1 证伪条件判断
              ├─ H1 成立且有收益 → W2（剩余锁热点）→ W3（翻译降本）
              └─ H1 不成立      → W4（Guest 同步闭合），W2 暂停
W4 只做分析，可以从 W0 结束后与 W1 并行
W5 本轮只做诊断，不改行为
```

每个阶段的流程都是：实现 → 主机测试 → Mac MoltenVK 与 Odin3 Turnip probe（**显式重新链接 probe**，Gradle 构建 APK 不会重链）→ 安装 → 同进程 ABAB → Litep → 报告。

**停止条件（所有阶段通用）**
- 同一阶段最多两轮"实现 → ABAB"迭代。两轮都没达标就写报告停止，不要第三轮调参。
- 出现 fatal signal、device lost、`bad_weak_ptr`、断言失败、明显缺绘或黑屏：立即关闭新开关，保留现场（日志、精确 ELF 符号化的栈），写报告停止。
- 配置一致性检查失败（见 §6）：保留字段差异，停止当前 A/B。

---

## W0：诊断与基线

**目的**：拿到"谁在等谁、等多久、锁被谁持有"的直接计数，不再依赖 Litep 父范围反推；同时固定线程角色。

改动：
1. **`frontlock_status` debugbus**（默认关闭，`on|off|status|reset`）。在 `FrontLock` 构造函数（`nvn_vulkan.cpp:202`）中：
   - 取得锁后（`depth==0`）把当前 API 名指针写入全局原子 `front_holder_api`，`release()` 时清空。
   - `try_lock` 失败时，在等待前读一次 `front_holder_api` 作为本次等待的"持有者"，用 `steady_clock` 计时。
   - 按 **(等待方 API, 等待方 Guest 线程 handle, 持有者 API)** 累计次数、总 ns、最大 ns，放在固定 128 项的表里（静态字符串指针去重，溢出计入 `other` 并计数）。
   - 线程**不用 TID 标识**（每次运行都会变），用 Guest 线程 handle（例如通知主线程的 `0x1d0027`），并可用现有 `threads` debugbus（`mng.guest-threads.v1`）对照优先级和核掩码。
2. 拆分 `ordered_fallback_reason == 5`：子原因枚举（shader variant 缺失 / binding 不支持 / 可写 storage / 范围或预算超限 / 别名 / 其他），以及首次失败的 stage、binding、请求字节和累计预算。只计数和记录首次现场，不逐 Draw 打日志。（供 W5 使用；有序模式下才会触发，基线测量时不开有序。）
3. 现有计数器补充每帧口径：`images_epoch` 增量、图像地址索引重建次数、`texture_initializations` / `texture_finalizations` 增量。W1b 会改变 release 的时机，需要这些作对照。
4. **基线测量**：当前已安装的 APK 加上 W0 改动后的新 APK，同进程，现有开关不变。4 个 30 s 未录制窗口，`frontlock_status` 在其中两个窗口开启（ABAB 形式，顺便测诊断自身的开销），然后取 3 s Litep（Guest tags + `profiler_address`）。

W0 报告必须回答：
- R、P、M 分别是哪个 Guest handle；P 是否就是 M。
- 持有者为 `nvnQueueSubmitCommands` 的等待，在总等待中占多少（按等待方 API 列出前 5 名）。
- R 的 `ReceiveMessageQueue` 等待（ms/源帧）与其通知者的锁等待在时间上是否重叠（用 `profiler_address` 的等待链，不靠时间接近推断）。

验收：
- `frontlock_status` 开启时，帧率与关闭时相比差异小于 1%（ABAB 合并）。
- 两次窗口中，按等待方 API 排序的前 3 名一致。
- Mac 和 Android 构建、lint、既有测试全部通过。

---

## W1a：无引擎依赖的去锁

改动（均受新开关 `publication_queue` 控制，与 W1b 共用；开关为 0 时行为与现状逐字节一致）：
- `nvnTextureCompare`（F10）：去掉 `LOCK`。
- `texture_initializations`、`texture_finalizations`（F9）以及本阶段新增的计数器全部改为 `std::atomic`，memory order 用 relaxed。
- 逐个排查 `nvnTextureGet*`、`nvnTextureGetViewOffset`、`nvnTextureGetStorageSize` 等 getter，确认没有任何一个经过 `FrontLock` 或进入引擎；有的话列出来，本阶段不改。

W1a 很小，不单独做 ABAB，与 W1b 合并测量；但需要单独的主机单测（Compare 的结果与改动前逐例一致，包括"零初始化槽位返回 0"这一 BOTW 依赖的行为）。

---

## W1b：发布队列（主线）

### 设计

新增 `porting/runtime/publication_queue.h`（不依赖引擎，可单测）：

- 一个有界 FIFO，带独立的 `std::mutex`。锁序：**持有该互斥时不得获取任何其他锁**；执行引擎操作时不持有该互斥（先整体取出，再在锁外按顺序执行）。
- 元素是值类型：`RetireTexture{tex_t}`、`RegisterTexture{dpool_t,int id,tex_t,view_t,bool has_view}`、`RegisterSampler{dpool_t,int id,samp_par}`。
- 容量上限放 `runtime_policy.h`，建议 4096 项。**满了就让调用方走原有的 `PUBLISH_LOCK` 同步路径**（先 drain 再执行本次操作，保持顺序），并计数 `publication_overflow`。不丢操作，不无界阻塞。
- `drain(execute)`：只能由**持有 `front_mutex` 的线程**调用，取出全部元素后按入队顺序调用对应的 `xbg_*` 引擎函数。

调用方（仅在 `publication_queue=1` 且 `translation_owner` 为空时）：

| API | 新行为 |
|---|---|
| `nvnTextureInitialize`（`:1021`） | 不取 `front_mutex`。校验、布局、取 serial、`mapped_gpu_ranges.include`、`write_texture` 照旧；旧 identity 的 release 改为入队 `RetireTexture` |
| `nvnTextureFinalize`（`:1141`） | 不取 `front_mutex`。入队 `RetireTexture`，再 `write_texture(t,{})` |
| `nvnTexturePoolRegisterTexture`（`:1608`） | 不取 `front_mutex`。`translate_view` 和 `descriptor_snapshot` 只读 Guest 存储（F7、F8），然后入队 `RegisterTexture` |
| `nvnSamplerPoolRegisterSampler*`（`:1631–1635`） | 同上，入队 `RegisterSampler` |

Drain 点（**全部在持有 `front_mutex` 之后、调用任何引擎函数之前**）：
1. `FrontLock` 构造函数中取得锁且 `depth==0` 之后，无论 `drain` 参数是 true（`LOCK`）还是 false（`PUBLISH_LOCK`）都执行。这一处覆盖了 Submit、Present、Flush 冲突路径、Invalidate、WriteTexels、FlushTexels、pool/queue/window finalize 等全部持锁路径。
2. 额外再保险一次：串行模式的 `nvnQueueSubmitCommands` 在调用 `xbg_submit` 之前（防止以后有人改动 FrontLock 时漏掉）。
3. `nvnMemoryPoolFinalize`（`:787`，内部调用 `invalidate_pool_textures`，`:822`）、`nvnTexturePoolInitialize`（`:1587`）、`nvnDeviceFinalize`（`:609`）、引擎 shutdown 之前。前三个都走 `FrontLock`，已被第 1 点覆盖，写测试证明即可；shutdown 路径要单独确认。
4. 动态关闭开关（`publication_queue on→off`）时，在持锁状态下 drain 一次再切换。

### 必须保持的不变量（每条都要有测试）

| # | 不变量 | 测试方法 |
|---|---|---|
| I1 | 同一线程 `Initialize(新) → Register(新) → Submit`：引擎依次看到 retire(旧)、register(新)，然后才是 submit | 主机单测，用记录型假引擎检查调用顺序 |
| I2 | 每次 Initialize 产生新的不可变 serial；每个旧 identity 只 retire 一次；`texture_finalizations` 与 retire 次数对账一致 | 主机单测加 probe 计数检查 |
| I3 | 新旧 identity 指向同一 `mem` 时，引擎处理新 identity 的第一条命令之前，已经看到旧 identity 的 retire | probe：同地址反复 init/finalize 夹杂 submit，逐像素读回 |
| I4 | 已录制命令、描述符快照、在途 GPU 工作仍持有旧 image 引用；retire 仍经过 `retire_object` 和 fence 延迟销毁 | probe：录制后 finalize 再 submit，读回正确 |
| I5 | pool finalize 或重新初始化之前，队列中不存在指向它的 register/retire | 主机单测加 probe |
| I6 | drain 中某个操作抛异常时，剩余元素不丢失：要么全部执行完再上报，要么明确 fatal，不允许静默丢弃 | 主机单测（假引擎注入异常） |
| I7 | 开关为 0，或者 `translation_owner` 非空时，行为与现状逐字节一致 | probe 双份运行，比对全部读回与计数 |
| I8 | 多个 Guest 线程并发 Initialize/Finalize/Register 时，队列顺序与"队列互斥的获取顺序"一致；与原先 `front_mutex` 提供的全序等价 | 主机多线程压力测试，在 Mac 上开 ASan/UBSan/TSan（ASan 此前在本机启动超时，若仍超时如实记录，但 UBSan 必须通过） |

### 观测（经 `renderer_publication status|on|off` 暴露）

`publication_queue`、`enqueued`、`drained`、`overflow`、`max_depth`、`drain_ns_total`、`drain_ns_max`、`drain_calls`，以及 W0 补充的每帧 `images_epoch` 增量和索引重建次数。

预期的副作用（需要测量，不要预设）：release 被批量处理后，索引重建可能变少，也可能把重建集中到 Submit 前，从而让翻译变长。

### W1b 验收

主机和 probe：上表 I1–I8 全部通过。既有测试全部通过（Python 回归、`live_ranges_test`、`conservative_ranges_test`、`ordered_executor_test`、`render_record_test`、`image_storage_test`、`vulkan_policy_test` 等）。probe 回归继续覆盖百万次临时对象循环、4096 个并发录制器、66000 次纹理录制回收。Android lint、Mac Metal/Vulkan 构建通过。

实机（同进程 OFF/ON/OFF/ON，每段 30 s，全部采样关闭；之后各取 3 s Litep，`frontlock_status` 只在 Litep 段开启）：

| 指标 | 基线参考 | 通过条件 |
|---|---|---|
| M 在 `nvnTextureInitialize` 上、持有者为 Submit 的锁等待 | 约 7.7 ms/源帧 | ≤ 基线的 5% |
| P 的锁等待（W0 确定的 handle） | W0 实测 | ≤ 基线的 20% |
| R 的 `ReceiveMessageQueue` 等待 | 约 20 ms/源帧 | 有下降，报告具体数值（H1 的直接证据） |
| R 的 Translate（elapsed 与线程 CPU） | 约 28 ms | 上升不超过 1 ms；drain 耗时单独列出 |
| 整帧 FPS（ON 合并 / OFF 合并） | 约 21 | **≥ +3%**，且两对中 ON 都高于相邻 OFF |
| `publication_overflow` | — | 稳态为 0；另做一次加载过渡测量并记录 |

必须附"等待转移表"：M、P、R 及其他 FrontLock 等待前 5 名线程的锁等待，M 的 `WorkerMgr::sync`（`main+0x1507e78`）和 `postCalc_` 下的 SVC 0x34 等待，R 的 Translate 与消息等待，以及 drain 耗时。只要有某项上升抵消了收益，就判定**未通过**。

未通过时：代码保留（开关默认 0），报告写清楚等待转移到了哪里，然后进入决策点 D1。

---

## 决策点 D1（W1b 之后，必须在报告里明确写出走哪条分支）

| 观察 | 结论 | 下一步 |
|---|---|---|
| P/M 锁等待基本消失，R 消息等待下降，FPS ≥ +3% | H1 成立 | 把 `publication_queue` 默认值改为 1（单独提交），进入 W2，然后 W3 |
| P/M 锁等待消失，R 消息等待不变，FPS 无变化 | H1 不成立：生产者被别的东西卡住 | 进入 W4，W2 暂停 |
| P/M 等待转移到其他持锁 API（`frontlock_status` 能看到新的前几名） | 串行环还有别的入口 | 进入 W2，只处理新出现的那个 API |
| Translate 明显变长（drain 或索引重建集中） | 收益被翻译线程吃掉 | 先把 drain 改为有界的"每次持锁最多处理 N 项"，或把索引重建延后，重测一次；仍不行则进入 W3 |

---

## W2：剩余前端锁热点（只处理 `frontlock_status` 里排进前 3 的 API）

候选及现状：

| API | 现状 | 方向 |
|---|---|---|
| `nvnMemoryPoolInvalidateMappedRange`（`:897`） | 无条件 `LOCK`，非别名池调 `mng_nvn_invalidate_range` | 用 `flush_dependencies().match()`：没有 `Writeback|Report|Event|Unknown` 交集时只做 acquire fence，不取锁；有交集或别名池时走原路径。参考 Mac `mtl_pool_invalidate` 先查交集再写回的做法 |
| `nvnTextureFlushTexels`（`:1129`） | `LOCK` 加 `xbg_texture_invalidate` | 评估改为入发布队列（新增元素类型 `InvalidateTexture`），顺序语义与 W1b 相同 |
| `nvnTextureWriteTexelsStrided`（`:1087`） | `LOCK` 加 `xbg_legacy_finish()`，相当于全局 finish | 改动大：需要按目标 image 的在途生产者等待。先写单独的设计文档，经评审后再做，本轮不实现 |

每个 API 单独一轮 ABAB，不合并测量，否则无法归因。验收模板与 W1b 相同，FPS 门槛为 +2%。

---

## W3：翻译降本（Draw 加 read preflight 约占翻译的 87%）

在 W1b 或 W2 达标之后再做。串行环打断后，翻译线程 CPU 就成为主要上限。

1. **先测再改**：用 `profiler_detail` 低频抽样，把 Draw 拆成 descriptor 命中维护、UBO snapshot、image lookup/prepare、vertex/index snapshot、命令编码；同时用 detail 关闭的相邻窗口测量扰动，并在报告中写出扰动百分比。已知参考：抽样 Draw 内约 7.48 次 Snapshot；preparation-plan 开或关时，descriptor 和 snapshot 的工作量没有明显差异。
2. **只优化排名前两位的项**。候选方向（需要数据支持后再选）：
   - read preflight 先按"当前 pending 输出的范围和代次"判断是否相关，再遍历绑定；同一资源版本复用已证明无冲突的结果；遇到新写入、别名或注册变化立即失效。
   - 把现有 256 项 read-range cache 和 preparation plan 统一到同一套依赖版本号，不要再叠加互相独立的缓存。
   - 同一 Draw 内同一资源只快照一次。
   - descriptor 命中时只刷新真实变化的动态偏移和内容版本，保留对"同地址内容改变"的检测。
3. 验收：Translate 线程 CPU 下降 ≥ 8%，同时未录制 FPS ≥ +2%（ABAB）。报告中列出扫描 binding/range 的次数、复制和 hash 的字节数、descriptor 重建次数、PSO 创建次数。所有范围、别名、同地址内容改变的回归测试通过，probe 读回一致。

---

## W4：Guest 同步关系闭合（纯分析；W0 之后即可开始，不改游戏语义）

要回答的三个问题：
1. M 每帧约 19 ms 的 `WorkerMgr::sync`（调用点 `main+0x1507e78`，返回偏移 `0x1507e7c`；当前 1.6.0 反汇编显示其调用 `main+0x13a03d0`）中，有多少是 M 自己在执行队列任务，多少是空等。
2. 慢任务叫什么名字、在哪个 Guest 线程执行。已解析出 25 个任务名位置（`ModelScene::calc`、`SystemTask::calcFrame`、`renderer-0/2`、`ParallelGsysCalcModelQueueJob`、`ksys::MessageDispatcher` 等），但还没有绑定到工作线程的执行起止。
3. 这些任务的执行线程是否出现在 `frontlock_status` 的等待方列表中，即是否被 NVN 前端锁间接阻塞。要有因果链（等待链或 token），不能只凭时间重叠。

方法：已有的 `profiler_address` 等待链加 12 个 Guest 标签；必要时新增**最小集合**的窄标签（IDA 验证，见 §3）。新增后重新测量开销：之前全量标签开启时，源帧间隔增长约 22%，这类数据不能作为无插桩基线。

产出：`ODIN3_GUEST_SYNC_<日期>.md`，包含结论、原始 SQL/查询、窗口边界和未知项。**不得**因为"主线程在等"就修改游戏的同步语义或给游戏打补丁。

---

## W5：有序翻译覆盖（本轮只做诊断）

- 用 W0 拆分出的 reason 5 子原因，列出阻断 `OP_DRAW_IDX`（handle 0、offset 12560）的前几名子原因及占比。
- 只有在 W1–W3 完成并复测后，翻译线程仍然是明确的单线程上限时，才提出"提交侧值型绑定状态加分段准入"的设计文档，评审通过后再实施。
- 本轮禁止：逐个放行 opcode、扩大整批快照预算、把有序翻译默认打开。

---

## 5. 期望收益与预算口径

| 阶段 | 可量化的上限依据 | 现实门槛 |
|---|---|---|
| W1b | M 在 Initialize 上的锁等待约 7.7 ms/帧，加上 P 的锁等待（W0 测得）；整个串行环打断后，帧时间下限约 28 ms | ≥ +3% |
| W2 | 每个 API 的锁等待（W0 测得） | 每项 ≥ +2% |
| W3 | 翻译约 28 ms/帧，其中 Draw 加 preflight 约 24 ms | 线程 CPU −8%，FPS +2% |

单线程等待消除不等于帧时间收益；这张表只用于判断"是否值得做"，不得作为报告中的收益声明。

---

## 6. 通用测量规范

- 脚本：以 `android/tools/benchmark_image_flush.py` 为模板，新增 `benchmark_publication.py`（W2 每个 API 一个子命令）。沿用 `validate_scene.py` 自动读最新存档，门槛为 Draw ≥ 1000、有 Dispatch、没有新建 PSO/shader。失败时保存结构化记录。
- 配置一致性：沿用 `stable` 字段清单，并加入新开关。A/B 之间任何字段变化都中止，并**保存具体字段差异**（上一轮首次中止时没有保存差异，无法归因）。
- 指标：成功 native present 的增量除以采样时长；合并统计用总帧数除以总时长；每项至少 4 个窗口（ABAB）。差异小于 2% 一律写"未证明"。
- Litep：只用公开 reader（`load_prof` / `open_trace_index` / `query_prof_sql`）；窗口取连续完整的 source frame；每份 PROF 记录 SHA、PID、chunk 数、skipped、pairing error、不完整 GPU zone 诊断。GPU/CPU 时钟对齐仍为 unknown，不做跨时钟结论。
- 解读：scope 时间是包含阻塞和抢占的 elapsed；嵌套 scope 和不同线程的时间不得相加；记录 thermal 状态。

## 7. 报告模板（每个阶段一份，中文，放在 `porting/nvn_vulkan/`）

1. 结论（一句话：通过或未通过，FPS 合并值，D1 分支）
2. 改动范围与开关（包括默认值）
3. 身份：设备、APK SHA-256、PID、Turnip SHA、main Build ID、配置字段快照
4. ABAB 原始表：每段的呈现数、秒数、FPS
5. 等待转移表（§W1b 规定的各项）
6. 计数器：队列、溢出、drain、索引重建、`images_epoch`
7. 测试：主机、Mac probe、Odin3 probe 的通过数与日志路径；未通过或未运行的如实列出
8. 完整性：PROF 诊断、数据保全（78 个文件的哈希）、最终设备状态
9. 未知项与下一步
10. 证据路径与 `change.diff` 路径

同步更新 `RUNTIME_POLICY.md`、`nvn_vulkan/README.md`、`android/VALIDATION.md`（追加本轮条目，风格与现有条目一致）。

## 8. 明确不要做

- 不要继续扩大 Flush 放行范围来减少锁等待：已证明等待会转移，本轮要处理的是转移后的入口（W1b）。
- 不要把 Initialize/Register 直接从 `front_mutex` 中拿出来却仍然同步调用引擎（F5）。
- 不要增加 PSO worker、扩大输入快照预算、逐个放行 opcode、默认打开有序翻译。
- 不要把 Mac 的 FPS、`BOTWRecompAndroid` 上游自报的 FPS、标题界面的 30/60 FPS 当作目标或证据；场景基线约 21 FPS。
- 不要用合成 probe 通过来代替整场景正确性，也不要用截图循环代替自动读档脚本。
