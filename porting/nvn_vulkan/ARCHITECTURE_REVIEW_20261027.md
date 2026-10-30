# BOTW Odin3：counter report 之后的架构与性能复核

日期：2026-10-27。

## 结论

下一轮优先处理 **GPU 资源范围依赖的表示和 Flush 的锁边界**，同时补齐 Draw 准入失败的具体原因；随后再把真正可并发的提交交给翻译线程。不要继续以增加 PSO warmup 线程、扩大输入快照预算或逐个放行 opcode 为主要优化路线。

当前有两类不同的问题：翻译线程运行时有大量 CPU 工作；其他 Guest 线程又被翻译期间的前端锁阻塞。只移动翻译线程，或者只增加一层状态缓存，都不足以证明流水线改善。

这是源码和已有实机证据的重新审查。本轮未修改运行时代码、未重新安装或启动游戏、未修改设备设置和存档。Odin3 上应用目前未运行；已只读核验安装 APK 与本地 APK、上一份报告的 SHA-256 完全一致：

`0bfb2ed42694cf87d9a82a4f53991bd3112851aade1114a17bc7729445c40b74`

## 已独立核对的事实

### 有序翻译还没有覆盖真实场景

[最新报告](ODIN3_COUNTER_REPORT_20261027.md) 的原始 `cadence-summary.json` 确认：同 APK 无插桩串行加权 **20.938 FPS**，有序 **19.452 FPS**，观测差异 **−7.100%**。两个有序窗口分别有 909、891 次同步回退，异步 Translate 均为零。

当前稳定阻断点是 `OP_DRAW_IDX`，handle 0、offset 12560，reason 5。它包含 shader variant、binding、可写 storage、范围／预算和别名等多种失败，尚未证明具体是哪一种。counter report 支持解决了此前 offset 8 的首个阻断，但没有实现游戏中的异步翻译。

有序窗口的 `ordered_drains` 增量约 9.2 万；这只是 drain 调用数，其中可以包含空队列检查，不能解释成 9.2 万次实际阻塞。额外预扫描和队列管理是代码中存在的开销，但不能把观测到的全部 7.1% 差异直接归给其中一项。

### 翻译段主要在消耗 CPU，而不是等待 fence

通过 Litep 公开 `open_trace_index`、`load_prof`、`query_prof_sql` 重新读取 `basic_a`：

- PID 10383，Translate TID 10640；present marker TID 10737。
- 源文件 SHA-256：`5de8f4f97517bb8136e7988f9f79f443abdde28632c315d3567ad6bcd295fcd9`。
- 完整帧窗口：`426271785084818..426274729284975 ns`，60 个完整 source frame 间隔。
- 新 aggregate 验证 payload complete、scope pairing errors 0、skipped chunks 0；仍保留窗口边界开放 scope 和一条 incomplete GPU zone 诊断。
- 59 组 Translate 时钟计数：平均 elapsed **28.246 ms**，线程 CPU **27.919 ms**，有效率 100%，CPU／elapsed **98.84%**。

这里的 CPU 时间来自捕获的线程 CPU 时钟，不是将 scope elapsed 当成 CPU 时间。数值包含采样自身的成本，不能直接称为无插桩运行成本；也不表示整颗 CPU 已满载。

同一窗口内，按 60 个完整 source frame 间隔归一化：

| 项目 | ms／source frame | 解释 |
|---|---:|---|
| Translate inclusive | 27.792 | 包含下面的翻译子阶段 |
| Draw inclusive | 19.574 | 平均单 Draw 5.362 μs；窗口内 219049 次 |
| CPU read preflight | 4.669 | 平均 1.274 μs，219946 次；位于 Draw scope 之前 |
| Translate self | 3.368 | 没有被其已记录子 scope 覆盖的部分，不能直接命名为纯解码 |
| Translate 线程的 GuestCondition | 20.395 | 翻译段之外的消息等待，不是 GPU fence |

Draw 加 preflight 已占 Translate inclusive 的约 **87.2%**。当前 detail 关闭，尚不能把 Draw 内部的 descriptor、资源刷新、snapshot 和 Vulkan 编码成本进一步精确排名。不能把父子 scope 相加，也不能把所有线程等待相加得到帧长。

热场景的原始无插桩窗口要求并验证无新增 shader／PSO；[preparation-plan 对照](ODIN3_PREPARATION_AB_20261027.md) 也未证明 FPS 收益。Android 生成引擎的实际编译命令使用 **`-O2`**，不能仅因 APK 名称含 Debug 就归因于未优化编译。

### Flush 仍会把 Guest 工作挡在整批翻译后面

现有 `ConservativeRanges` 是只增不减的历史范围表。纹理初始化、GPU 写回、report、event 等都会登记；没有随实际依赖退休删除范围。它适合防止漏检，不等于当前尚未完成的资源依赖。

在 `basic_a` 完整窗口内，**662 次 `NVN.Flush.RangeMatch` 全部为 Overlap（1）**，无 Clear；记录的 pool flags 均为 36。保守路径获取 `front_mutex`，此处只做 release fence，没有按实际相交的提交进行选择性等待。同步 Translate 又在整个执行期间持有同一把锁。

| TID | FrontLock 等待次数 | 累计等待 ms | 平均 ms／次 |
|---|---:|---:|---:|
| 10725 | 79 | 1245.58 | 15.767 |
| 10565 | 26 | 428.58 | 16.484 |
| 10653 | 46 | 447.44 | 9.727 |
| 10652 | 49 | 305.95 | 6.244 |

这些并行线程的等待不能相加当作可节省时间。选择一个 300 ms 原始区间复查，TID 10565 的 4 次完整锁等待共 80.302 ms，其中 79.990 ms 与 Translate 重叠。源码证明了相同锁的持有关系；这支持“整批翻译阻塞 Guest Flush”，尚不证明移除其中某次等待就能获得等量 FPS 收益。

原报告的窄 Guest 捕获还确认：Translate 线程的 56 次完整消息等待均由 TID 10565 选中通知。因此该线程值得优先追踪。约 21.8 ms 的通知前等待不能直接命名为游戏逻辑计算；需要观察它在正确缩小 Flush 等待后是否缩短，以及是否出现等待转移。

### 当前异步准入方案比 Mac 参考更重

`mng_capture_submission` 持引擎锁，从执行侧 `state` 复制继承绑定，逐 Draw 解析 shader 和实际输入。它收集完整 vertex binding、纹理和附件范围，整批输入预算 32 MiB；任何不支持的输入或命令使整批同步回退。放宽预算不能修复所有权或 GPU→GPU 数据依赖。

成功准入也仍有以下边界：下一次 Submit 的 `LOCK` 会先排空前序翻译；Present 捕获前显式 drain，并等待此前窗口票据。因此“增加了 worker”不是已解除流水线串行化的证明。

Mac 参考中可复用的是三种机制：

1. 提交时保存易被 Guest 改写的 UBO 和延迟绑定；稳定资源保留对象身份和生命周期，不对所有纹理／顶点数据进行整帧深拷贝。
2. `mtl_pool_invalidate` 先查看当前待写回集合及真实范围交集；没有相交项就返回，有交集才进入 writeback 和 drain。
3. Submit／Present／资源释放按顺序排队，翻译票据和 GPU 完成票据分开，Acquire 等待对应 image。

Mac 的 shared/no-copy buffer 和 dirty tracking 依赖其具体内存设施。Android 统一内存不自动提供同等 Guest 页跟踪或 Vulkan 外部内存导入能力，不能直接删快照或照搬 Metal API。Mac 的异步开关源码默认关闭；本轮没有核验以前高 FPS 运行的该开关，不能声称 Mac 性能优势已经被证明来自异步。

## 建议的实施顺序

### 第一轮：先用串行模式收敛范围依赖和 Flush

- 增加有界诊断：Flush 的 pool generation、真实 backing 范围、命中的资源／提交、依赖类型、是否仍在途。reason 5 拆成具体子原因，记录 stage/binding、请求字节数／累计预算、首次失败位置；避免逐 Draw 大量日志。
- 区分 CPU 上传、待执行翻译读取、GPU buffer 写回、镜像 texture 写回、counter/event 输出。历史范围仅作未知情况的保守兜底；维护带 generation、alias backing 和提交序号的在途依赖表。
- 资源身份、引用和依赖发布必须先于异步可见，退休与重映射不得产生漏检窗口。普通 coherent CPU 上传按协议发布可见性；真正冲突才等待对应依赖。Invalidate／readback 保留真实写回，不把所有 Flush 改成无条件成功。
- 保持 ordered 默认关闭，先做独立 A/B，判断 Guest producer 的锁等待和帧间隔是否下降。

验收：同地址重用、部分范围重叠、物理／虚拟别名、CPU 覆盖较新内容、report/event、资源 finalize／remap 均保留正确性；真实无冲突 Flush 能走快路径，且等待没有转移到下一入口。不能仅看锁等待数字下降。

### 第二轮：减少每 Draw 的资源准备工作

- 用现有低频 `profiler_detail` 抽样分离 descriptor 命中维护、UBO snapshot、image lookup／prepare、vertex/index snapshot、编码；用包围它的 detail-OFF 窗口校验扰动，不直接放大全量插桩结果。
- preflight 先按当前 pending 输出的范围／代次判断是否相关，再遍历所需绑定。对于同一资源版本复用已证明无冲突的结果；遇到新写入、别名、注册变更立即失效。现有 256 项 read-range cache 与 preparation plan 应统一使用依赖版本，避免继续叠加相互独立的缓存。
- descriptor 命中应尽量消费紧凑稳定计划，仅刷新真实变化的动态偏移、内容版本和依赖；保留同地址内容改变的检测。优先减少查找／重复访问次数，再考虑将独立准备工作放进 Foundation JobSystem。

验收：真实 Translate 的线程 CPU 时间和无插桩帧间隔同时改善；统计扫描 binding／range 次数、复制／hash 字节数、descriptor 重建数、PSO 创建数。仅增加 cache hit 数不算成功。

### 第三轮：让有序翻译真正覆盖 BOTW

- 在提交侧维护独立的值型绑定状态，避免为读取继承状态而等待执行侧完成；命令及资源元数据持稳定代次引用。
- 不把全部 shader GPU 写入判定为必须同步：可证明的 GPU→GPU 依赖留在 GPU 时间线上；CPU 可变输入才快照，CPU 可见输出继续按 fence／invalidate 生命周期处理。
- 若整批准入确实被少量 CPU 可见操作阻断，可在有明确定义的同步点分段；必须保留状态继承、render pass、barrier、query 及 alias 顺序，不能任意按 Draw 数拆包。
- Present 对已经存在的 host image 应排队引用对应资源和代次；CPU-backed 初次上传才需要输入捕获。只在相关 Acquire、SyncWait、读回、销毁／重映射处等待必要票据。

验收：稳态异步 Translate 覆盖率必须实测为非零并足够高，记录真实执行而非仅 admitted 数；capture 成本、队列深度、背压、RSS、输入延迟和两端等待都要进入 A/B。CPU 同时忙起来但 FPS 不涨也不算成功。

## 代码入口与复核材料

- [`nvn_vulkan.cpp`](nvn_vulkan.cpp)：`nvnMemoryPoolFlushMappedRange`（829）、`FrontLock`（184）、`nvnQueueSubmitCommands`（1691）、`nvnQueuePresentTexture`（1877）。
- [`conservative_ranges.h`](../runtime/conservative_ranges.h)：历史范围表与 fail-closed 行为。
- [`ordered_submission_engine.inc`](ordered_submission_engine.inc)、[`submission_inputs.h`](../runtime/submission_inputs.h)：输入预扫描、整批准入与快照预算。
- [`read_preflight_engine.inc`](read_preflight_engine.inc)、[`read_range_cache_engine.inc`](read_range_cache_engine.inc)、[`preparation_plan_engine.inc`](preparation_plan_engine.inc)：每 Draw 范围和绑定处理。
- [`prepare_engine.py`](prepare_engine.py)：生成引擎的适配入口；不要直接编辑构建目录中的 `xbg_game.cpp`。
- [`hle_nvn.c`](../../games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c)：Mac 参考 Submit 和有序队列（2055–2238）。
- [`nvn_mtl.m`](../../games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m)：Mac UBO capture（1395）、pool invalidate（2565）。

本轮公开 Litep SQL、原始结果及代表区间计算保存在：

`/Users/bytedance/workspace/native-switch-study/android-translate-20261026/audit-20261027/reanalysis.json`

两次全窗口 interval join 触及公开 SQL 5 秒上限，已改为有界 300 ms 查询，不绕过公开 reader。没有新增完整 GPU／scheduler 捕获，不能据此宣称 GPU 饱和度或完整 CPU→GPU 关键路径。先保持现有串行配置、720p 手持／1×、相同驱动和存档，逐轮验证上述变更。
