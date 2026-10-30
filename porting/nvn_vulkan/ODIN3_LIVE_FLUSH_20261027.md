# Odin3：活动范围依赖与 Flush 全局锁

## 结论

本轮将普通内存池的 Flush 判断从历史范围集合改为当前输出所有权，已在共享 Vulkan 实现落地，并安装到 Odin3。Mac MoltenVK 与 Android Turnip 使用同一份代码和策略。三个 Guest 线程的无关全局锁等待显著下降，但四轮同进程 A/B 的合并帧率只有 **20.775 → 20.949 FPS（+0.84%）**；第二对结果略微下降，因此**没有证明稳定的整帧提速**。

剩余瓶颈仍包括约 27–28 ms 的同步翻译，以及一个线程每源帧约 20–21 ms 的图像范围冲突等待。其他线程省下的时间部分转移到 Guest 地址／条件同步，不能将跨线程累计等待相加当作帧时间收益。

## Mac Metal 对照与实现边界

Mac 的 `nvnMemoryPoolFlushMappedRange` 进入 `mtl_pool_invalidate`：在短锁内筛选当前 `g_wb` 中相交的写回项，没有交集就返回；有交集才进入 `wb_write_list`，执行必要的翻译排空、GPU 等待和新旧内容判断。参考位置为 `games/totk/third_party/d2r_gfx/gfx/nvn/hle_nvn.c` 与 `nvn_mtl.m`。

旧 Vulkan 的 `ConservativeRanges` 保留历史纹理／输出范围，地址曾经命中后，即使对应输出已经消失，Flush 仍可能等待持有 `front_mutex` 的整批同步翻译。本轮借鉴 Mac 的“先判断活动范围”组织方式，未复制其 Metal API，也未修改只读 D2R 参考检出。

新增 `porting/runtime/live_ranges.h`：

- 图像 GPU 内容、storage 写回、counter report 和 event 分别持有范围租约；复制所有者共享租约，最后一个所有者释放后退役。
- 查询只使用独立短锁，不读取正在变化的引擎容器，也不获取前端全局锁。
- 普通池没有活动输出交集时，只执行 CPU 发布屏障；有交集时仍走原有锁路径。
- 虚拟／物理别名、范围溢出、容量上限与未知所有权保守处理。有序翻译模式仍回退旧路径，尚未证明排队输入的完整所有权。
- GPU 写过的图像不能仅在 fence 完成时删除租约：宿主图像可能仍是权威内容。租约保留到 CPU 合并重置状态或物理图像销毁，避免过早放行。

`prepare_live_flush.py` 将输出所有权接入构建目录的 D2R 引擎副本；校验固定源码替换点，Mac／Android 共用。`runtime_policy.h` 统一定义 `MNG_LIVE_RANGE_FLUSH=1`，`renderer_schedule flush on|off` 仅用于同进程对照。

**本轮不是完整的 Mac 写回机制移植。** GPU 等待、写回及回收规则没有被省略；相交范围仍可能被保守地扩大成整批翻译等待。尤其是持久图像内容与当前在途生产者尚未分层，这正是下一步需要处理的边界。

## Odin3 同进程 A/B

设备 `6edb0593`，AYN Odin3；包 `org.macnative.android.botw`，同一游戏 PID `2223`。BOTW 1.6.0 主模块 Build ID `8E9978D50BDD20B4C8395A106C27FFDE`。设置保持手持 720p、internal scale 1x、4 个 PSO worker、preparation plan 开启、有序翻译关闭；其他共享 Vulkan 开关逐轮核验不变。

新 APK SHA-256：`a9fad5f1449d63dce9fc5491fef8caf23d9a733b38ab117832ab3d5b3c1bc582`。与安装前 APK 比较，归档项只改变 `lib/arm64-v8a/libmacnative_platform.so`。Turnip 身份为 `e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`。

由 `validate_scene.py` 自动读入当前存档，要求成功读档、持续呈现、场景 Draw／Dispatch 与 shader 预热门限。脚本没有输入相机或移动；语义场景沿用当前对照存档，**门限不能独立证明地图名或精确角色姿态**。

```sh
python3 -I android/tools/benchmark_translation.py \
  --serial 6edb0593 --expected-pid 2223 \
  --output <new-evidence-directory> \
  --seconds 45 --capture-seconds 3 --flush-only
```

每轮先稳定 3 秒，再测约 45 秒未录制帧率，随后另采 3 秒 Litep；FPS 是成功原生 Vulkan Present 计数率，不是显示合成器时间戳。测试结束恢复原设置。该模式不截图。

| 顺序 | 活动范围 Flush | 未录制 FPS | Flush 快路径次数 | 冲突路径次数 |
| --- | --- | ---: | ---: | ---: |
| OFF A | 关 | 20.675 | 0 | 旧路径 10,897 |
| ON A | 开 | 21.082 | 968 | 9,874 |
| OFF B | 关 | 20.875 | 0 | 旧路径 10,842 |
| ON B | 开 | 20.816 | 961 | 9,803 |

按实际计数／时间合并：OFF 20.775，ON 20.949。两次 ON 都只有约 8.9% Flush 能绕过锁，unknown 为 0。热状态均为 0，仍不能排除正常调度、动画负载及温度漂移。此 A/B 的两种模式都包含新范围表维护开销；它隔离的是放行策略，不是整份补丁相对旧 APK 的总成本。

## Litep：消除的等待与未消除的关键路径

使用公开 Litep `load_prof`、`index_trace`、`query_prof_sql`。每份原始 PROF、sidecar、PID 与 SHA 均保留。分析裁剪到连续完整的 source FrameMark 窗口，分别为 60／61／58／61 帧；不是将嵌套 scope 或不同线程耗时直接相加。

四份均完整解码、没有丢块、没有 scope pairing error。启动边界 unmatched end 数为 0／10／10／7，结束 open span 数为 44／49／58／54，已保留并按边界处理。每份都报告一个不完整 GPU zone，GPU/CPU 时钟对齐未知；本轮不据此声称 GPU 饱和、空闲或 CPU/GPU 因果关系。

| 指标 | OFF A | ON A | OFF B | ON B |
| --- | ---: | ---: | ---: | ---: |
| 每批 Translate elapsed，ms（counter） | 28.655 | 28.331 | 28.032 | 27.355 |
| 每批 Translate thread CPU，ms（counter） | 28.311 | 28.026 | 27.601 | 26.986 |
| Draw，ms／源帧（TID 2485） | 19.884 | 19.911 | 19.530 | 18.999 |
| Read preflight，ms／源帧（TID 2485） | 4.714 | 4.472 | 4.645 | 4.505 |
| Translate 外 GuestCondition，ms／源帧（TID 2485） | 20.391 | 19.810 | 22.302 | 20.067 |
| FrontLock.Wait，ms／源帧（TID 2371） | 8.357 | 0.001 | 10.712 | 0.013 |
| FrontLock.Wait，ms／源帧（TID 2501） | 7.096 | 0.023 | 4.541 | 0.022 |
| FrontLock.Wait，ms／源帧（TID 2503） | 4.722 | 0.067 | 3.907 | 0.057 |
| FrontLock.Wait，ms／源帧（TID 2653） | 21.217 | 21.175 | 20.237 | 19.797 |

`ThreadCpuValid` 全部为 1。翻译批次约 98.5–98.9% 的 elapsed 是该线程实际 CPU 时间；这不等于整机 CPU 利用率，也不包括驱动其他线程。Draw 加 preflight 仍占翻译约 86%，单靠减少其他 Guest 线程的锁等待不会消除这部分执行成本。

ON A／ON B 的 TID 2653 均记录 610 次 `NVN.Flush.LiveKinds=1`，即图像范围冲突，约每源帧 10 次；不是 report、event 或 storage 写回冲突。其全局锁等待基本未变。

等待转移也有直接观测：第一对中 TID 2371 的 GuestAddress 从 27.308 增至 33.540 ms／源帧；TID 2501／2503 的 GuestCondition 从 30.861／27.244 增至 35.317／33.492 ms／源帧。因此不能把已消除的 FrontLock 累计时间当作可兑现的 FPS 收益。

## 验证与保留

- Mac 完整 Vulkan runtime、Android APK 和 Android lint 构建通过。
- 6 项范围／策略／所有权 Host 回归、1 项 debugbus 调度回归、28 项 Python 回归通过；新范围表通过 ASan／UBSan。
- Mac 真实 MoltenVK GPU probe 的旧路径、活动范围路径、有序翻译回退三组通过，每组 404 次提交、178 次 draw、10 次 dispatch。包含像素、读回、报告、别名和资源生命周期检查；设备不支持的 depth bounds 未宣称通过。
- 安装前备份的 78 个文件在安装后哈希全部一致。游戏运行后的两次一致清单为 77 个未变，只有 `option.sav` 改变 4 字节，保留该变化，没有恢复／清空存档或缓存。
- APK、进程和 profiler 状态复核通过，无匹配到 fatal signal、device lost、断言失败或 `bad_weak_ptr`；游戏持续呈现并保留运行。没有以这组运行日志代替人工图像正确性验证。

## 下一步

1. 优先收窄 TID 2653 的图像冲突：建立有界 Flush 范围 → 图像身份／代次 → 实际生产提交的关联，分离“宿主保留权威图像内容”和“当前确实存在未完成生产者”。对照 Mac 的 freshness／writeback 决策，只等待相交的实际工作，不能在 fence 完成时直接丢掉权威内容，也不能把所有图像冲突强行当作 Clear。
2. 将相交资源交接与全局 `front_mutex` 脱钩，需要验证图像 CPU 合并、别名和待翻译命令的顺序。当前 ON/OFF 已证明无关线程放行，不代表这条主等待链已解决。
3. 单独优化 Draw 和 read preflight 的重复工作，再用同样的未录制 FPS + Litep A/B。热场景已经没有 PSO 构建任务，继续增加编译 worker 不是本轮数据支持的首选。

Git 外证据根目录：

`/Users/bytedance/workspace/native-switch-study/android-translate-20261026/flush-range-20261027/`

其中 `ab/result.json`、四份 `ab/*/*.prof`、`litep/evidence.json`、`litep/normalized.json`、`preservation-final.json` 保留原始结果。构建、合成测试、安装脚本、APK 和本轮差异位于 `android/build/nvn-live-flush-20261027/`，不纳入游戏资产或密钥。
