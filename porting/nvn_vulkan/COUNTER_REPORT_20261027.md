# Counter report 的受约束有序执行

日期：2026-10-27。

## 改造范围

上一轮 Odin3 的第一个异步阻断点是 `OP_REPORT_COUNTER`，handle 0、offset 8。本轮为它建立输出生命周期检查，而不是仅把 opcode 加入无条件允许列表。其余未知命令、可写 shader storage 和不完整输入仍同步回退。有序执行和 Guest 标记仍默认关闭。

本轮只改 MacNativeGames 的适配器、构建适配和测试；固定 D2R 第三方源码未修改。第一阶段准备缓存保持原实现。

## 输出约束

- 报告类型限于现有执行器支持的 timestamp、top timestamp、samples passed。
- 地址非零、16 字节对齐，完整 `NVNcounterData` 范围必须被唯一的普通注册内存池覆盖。
- 物理池、虚拟池、重叠池注册、越界输出均拒绝异步。物理别名需要 backing + offset 身份，不能用虚拟地址不重叠代替证明。
- 同一提交的任何捕获输入与报告输出重叠时拒绝异步；即使读发生在报告前，也保守回退。
- 报告输出与尚未退休的 GPU buffer writeback 重叠时回退。已有跨提交 pending report/input 重叠检查继续保留。
- 单次提交最多 4096 条输出记录，输出记录容量计入原有 64 MiB 入队预算；原有 32 MiB 输入上限不变。

输出不是普通输入快照，不复制旧结果来代替最终报告。当前边界覆盖 NVN 管理的池生命周期及已知物理／虚拟别名，不声称可侦测调用者私自释放存储、任意外部 mmap 别名或违反 API 生命周期的写入。

## 从准入到 GPU 写回

普通池初始化时建立不可变的范围与独立代次。提交捕获持有该代次；真正执行 report 时，将它保留进 `PendingReport`，因此提交对象销毁后证明仍在。原 `write_report` 在真正写回前核验代次仍有效且范围完整。

实际内存依然由调用者拥有，代次对象不是 Guest 分配的所有者。安全来自已有生命周期协议：

1. `MemoryPoolFinalize` 排空翻译，再完成 GPU 与 pending report 写回。
2. 之后才失效代次、注销池并释放适配器拥有的 backing。
3. 虚拟 remap 继续先完成 GPU。物理池初始化的 `MAP_FIXED` 路径也在复制旧 backing 前退休 GPU 工作，避免在报告写回前替换原地址。
4. 同一个池对象尚未 finalize 时，重复 initialize 被拒绝，不能偷偷替换代次。
5. shutdown 仍先关闭并排空执行器、完成 GPU，再失效剩余池代次。

不改变 timestamp query 的 pipeline stage、计时校准、samples prefix、结果退休顺序、GPU fence 或 CPU invalidate。Host translation ticket 依然不是 GPU 完成信号；`SyncWait` 的 poll/有限 timeout 也不改为立即成功。

## 可观测性

新增状态：

- `ordered_report_submits`：真正异步接收、含报告的提交数。
- `ordered_report_outputs`：上述提交中的报告命令数；不是 GPU 已完成数。
- `ordered_fallback_8`：报告类型、输出所有权／范围或记录预算不满足。
- `ordered_fallback_9`：报告输出与捕获输入／pending writeback 冲突。
- `ordered_last_opcode/handle/offset`：最近一次任意原因回退的扫描位置；末尾范围检查失败时只是最后扫描位置，不宣称该 opcode 自身致错。

原 `ordered_first_*` / `ordered_opcode_*` 仍只统计原因 4 的首个未知指令，不改旧字段含义。

## 缩窄 Guest 标记

`profiler_guest start` 现在只为条件等待／通知 SVC `0x1C`、`0x1D` 记录模块偏移和有界 FP 返回线索。不再为 SleepThread/yield、普通同步等待或 address-arbiter SVC 做这套昂贵记录；先检查 SVC 类型，再读取诊断 epoch。

真实 Enqueue、SelectedNotify、Wake、Resume 的 epoch、wait ID、逻辑等待者和 recording-generation 规则保持不变。这一轮没有按猜测的游戏函数或固定运行时地址过滤，也没有宣称覆盖纯 Guest 计算。基础 SVC profiling 仍保持原状。扰动下降须由新 APK 的 OFF/ON/OFF 实测判断，不能仅按减少的事件种类推算。

## 正确性验证

- 20/20 Host CTest 和共享 profiler 控制测试通过。
- 27/27 Python 测试通过。
- 当前 Xcode 21 工具链的 ASan/UBSan：executor、输出范围／代次／预算、真实 condition 路径通过。默认 Xcode 16.3 的 sanitizer 在 `main` 前出现初始化重入自旋；保留 stack sample，改用已安装的新工具链重跑，没有改系统默认工具链或移除 sanitizer。
- 三组真实 GPU 合成：串行、有序＋准备缓存、有序−准备缓存，均通过，404 submissions、178 draws、10 dispatches。
- 每组有序测试记录 6 个含报告的异步提交、9 个输出。暂停 worker 时输出仍是 sentinel；poll/有限 wait 不提前成功；invalidate 和池 finalize 都必须等待，随后能看到正确结果。测试还覆盖同地址不同池代次复用、未注册输出的同步回退、物理／虚拟池拒绝、报告／附件输入别名回退及 timestamp 顺序与 Guest 时钟一致性。
- `assembleBotwDebug`、`lintBotwDebug` 通过。

主机合成不等于 Android Vulkan validation 或 BOTW 全场景验证，不能据此宣称 FPS 改善。

## 交付身份

APK SHA-256：`0bfb2ed42694cf87d9a82a4f53991bd3112851aade1114a17bc7729445c40b74`。

相对上一轮 `dd7dba23…`，ZIP 内只有 `lib/arm64-v8a/libmacnative_platform.so` 改变，无新增／删除条目。构建与验证证据保存在 `android/build/nvn-counter-report-20261027/`，包含修改前逐文件快照、本轮 patch、源 SHA、测试日志及不可变 APK。未提交、未推送。

Odin3 实机验证已完成，详见 [ODIN3_COUNTER_REPORT_20261027.md](ODIN3_COUNTER_REPORT_20261027.md)：report 不再是首个阻断，稳态转为 `OP_DRAW_IDX` 输入安全检查回退，整批异步仍为零；串行 20.938 FPS、有序 19.452 FPS，未证明提速。窄 Guest 标记的短窗口扰动为约 7.17%，显式消息等待链保留。设备已恢复临时开关，78 个文件全部保留，仅 option.sav 4 字节自然运行变化，未恢复或覆盖。
