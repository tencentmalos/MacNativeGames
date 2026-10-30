# BOTW：提交槽、呈现与范围失效的串行等待

承接 [Pass 与翻译对齐](CONCURRENCY_20261026.md)。本轮继续使用同一份
BOTW 1.6.0（main Build ID `8E9978D50BDD20B4C8395A106C27FFDE`）、
复苏神庙存档、手持 720p、内部 1×、关闭垂直同步、120 上限、关闭 MFX/TAA。
原始证据位于仓库外 `native-switch-study/macos-base-20261025/concurrency-analysis-20261026/`。

## 已确认的问题

1. 原来的 1024 操作拆批不检查下一槽是否可用。两槽均有在途工作时，翻译需要先等待
   旧槽 fence，才能继续写入该槽的 arena。等待期间仍持有 front 和 engine 锁。
2. Guest 显式 Flush 已提交最后一批时，呈现为了给源图像添加一条布局转换，会重新取得
   NVN 槽。宿主本来已经拥有专用呈现命令缓冲，没必要再为这条转换等待 arena。
3. `nvnMemoryPoolInvalidateMappedRange` 无条件调用 QueueFinish，包括无关的 CPU
   读取范围。新增的 API 所有者 scope 确认：PID 16082 旧路径的 4 秒采集中，
   该 API 所在 Guest 线程累计约 511 ms 的 `NVN.Fence.Complete`。
4. 保守 Flush 仍有范围过宽问题：PID 4206 的基础采集里，3080 次保守 Flush 都是
   普通缓存池（flags 36），均因历史纹理范围重叠进入慢路径，并非 physical/virtual
   别名兜底。当前尚未删除这一同步契约。

Mac 的 `mtl_pool_invalidate` 在短锁内选择相交的待写回资源，随后兑现对应的写回；
并不把每个缓存维护调用都变成整个渲染队列完成。其 CPU 新旧数据合并规则仍有独立意义，
不能照搬“删锁”作为 Vulkan 镜像一致性的实现。本轮只收紧已经由 Vulkan 待写回、
报告、事件列表覆盖的 Invalidate，保留 Flush 的原有保守顺序。

## 实现与边界

- 自适应拆批：在原有 SetRenderTargets 边界查询下一槽 fence。如果仍未完成，继续在
  当前槽翻译，之后再尝试拆批。保留两槽、有界 arena、原有引用和退休规则；显式 Flush、
  arena 上限和实际读回不经过这个可延后分支。`VK_NOT_READY` 以外的错误仍然失败。
- 呈现源转换：宿主声明支持后，先提交当前录制槽，再由 `cfg.present` 在专用呈现
  命令缓冲记录源图像转换并提交到同一 VkQueue。全过程由 engine 锁保护，更新图像布局
  后才能允许下一次翻译。源 Image 的强引用一直保留到实际复制 fence 完成。
  未声明能力的宿主保持旧路径；声明后漏掉转换会显式失败。
- 范围 Invalidate：普通池只同步相交的 pending writeback / counter report。
  相交的待应用事件仍走完整完成路径；physical/virtual 池继续保守完成全部工作。
  不相交时不提交、不等待、不退休 GPU 槽。CPU 可见数据仍由原有 fence 退休和写回逻辑发布。
- Litep 新增 `NVN.Fence.RingReuse/Complete/...`、真实 `NVN.Slot.SubmitId` 与
  `NVN.Fence.SubmitId`，前端锁 scope 外层标明调用 API。CPU 等待原因可区分，
  但这些编号本身不提供 CPU/GPU 时钟校准。

没有启动新的异步 Guest 翻译 worker。命令副本已不可变，但 Guest UBO、间接参数与
描述符代仍需要完整快照契约，不能仅将持有 Guest 指针的命令扔到 JobSystem。

## 同进程 A/B

`renderer_schedule` 通过共享 debugbus 注册，Mac 与 Android 都接入同一控制回调：

```text
renderer_schedule status
renderer_schedule adaptive on|off
renderer_schedule batch 0..65536
renderer_schedule present on|off
renderer_schedule invalidate on|off
```

控制只在 engine 边界用 try_lock 应用；忙时返回 `emulation_session_busy`，不会让
诊断请求等待 GPU。这是诊断开关，不写启动配置。`present_capable` 单独说明宿主能力。
Litep 当前桌面 target 的 debug 透传仍返回仅支持 Android，因此这里使用 PID 校验过的
`macos/debugbus.py` 控制开关，采集、拉取与解析继续使用 Litep。

PID 21303 的同进程、同视角、无录制矩阵（每窗口约 6 秒，切换后先等待 2 秒）：

可执行文件 SHA-256 `b5b4a89e91dc46e321aa4513d4b352782bdfb88fc2b12f1b321090eb44a16694`，
Mach-O UUID `5DB388DD-A259-34C0-ADD1-674EDB3417AA`。`lsof` 确认仍加载上一轮固定的
MoltenVK 1.4.2（SHA-256 `8ea25172b62b40919bcb60d24c7d93970ac625921a38eb625e66d57f52e8ea8e`）。
两个存档 SHA 与上一轮一致；源码身份保留在 `direct-source-hashes.json`。

| 设置 | 成功呈现次数/秒 |
| --- | ---: |
| 旧路径，1024 拆批 | 54.19 |
| 三项优化开启，1024 | 65.00 |
| 只开启范围 Invalidate | 59.62 |
| 只开启呈现转换交接 | 52.85 |
| 三项开启，2048 | 63.42 |
| 旧路径重复 | 52.30 |
| 三项开启，1024 重复 | 65.97 |

保留 1024 自适应拆批。两次新路径的范围约 65–66，旧路径约 52–54，观察到约
20–26% 提升。单项呈现交接没有独立提升；它与拆批、范围完成之间存在依赖，不能把
单项计时收益直接相加。此前仅启用自适应拆批主要把约 2 ms 等待从翻译移到了呈现，
没有得到同等幅度的整体收益，相关失败假设和原始采集仍保留。

这些是完成呈现计数，不是经过验证的独立 Guest 图像帧数；动画与系统负载仍有波动。
没有宣称追平 Metal，也没有把 CPU elapsed scope 或 GPU query elapsed 当作利用率。

## Litep 闭环

最后两份基础采集来自同一 PID 21303，关闭 CPU/GPU detail；各约 6 秒。
先关闭三项优化，再全部打开。解析分别有 291 / 382 次完整 `NVN.Translate`，
源呈现 marker 也是 291 / 382。下表用各自完整翻译次数归一化，逐线程独立比较，
**不能将不同线程或父子 scope 的数值相加当作帧时间**。

| 物理线程 / 范围 | 旧路径 ms/翻译 | 新路径 ms/翻译 |
| --- | ---: | ---: |
| 翻译 1964514，`NVN.Translate` | 11.032 | 8.785 |
| 同线程，`NVN.Fence.RingReuse` | 1.943 | 0.000783 |
| 同线程，`NVN.FrontLock.Hold` | 11.065 | 8.814 |
| Flush 线程 1964679，`NVN.Flush.Conservative` | 8.651 | 6.167 |
| Guest 线程 1964521，Invalidate API | 2.193 | 0.000914 |
| 同线程，`NVN.FrontLock.Hold` | 2.241 | 0.0363 |
| 呈现 1964688，Prepare | 0.373 | 0.164 |
| 同线程，CompleteUnlocked | 5.856 | 5.609 |
| Guest 1964475，`NVN.SyncWait.GpuUnlocked` | 未观察到该范围 | 0.728 |

新路径呈现线程不再出现 `NVN.Fence.RingReuse`，没有把翻译里消失的等待原样搬到
呈现准备。真实 GPU 完成等待现在主要经已释放前端/引擎锁的 SyncWait 兑现；GPU 仍然
存在依赖，不能据此宣称“零等待”。保守 Flush 等待还剩约 6.17 ms/翻译，同步翻译
本身约 8.79 ms，仍显著高于上一轮 Metal 的约 5.30 ms。

原始文件与身份：

- 旧路径：`litep-direct-off/mng-direct-21303-file-1792990548861393-1.prof`，
  SHA-256 `59896f65f52ef24c4f0ce00d445bc15fa8dc6f0bd7f919c36c9206557dd33ee4`；
  Litep job `2f126889af1a42fd99ad19735467823c`。
- 新路径：`litep-direct-on-complete/mng-direct-21303-file-1792990704056174-3.prof`，
  SHA-256 `0f0ea1d3947505fda82b46e15c3837a0d67c470d9acc4abfc736f9617a80ac8c`；
  Litep job `555eef42a37241f8851e9563a8595142`。
- 对应 `direct-off/on-stage.json` 和 `direct-off/on-scopes.json` 保留完整性报告及 SQL
  聚合。两份容器和 payload 均完整，0 跳过 chunk、0 配对错误、无内部 recording 边界；
  采集切边分别有 19 / 20 个未结束 CPU scope。GPU 分别有 2 / 6 个 orphan time，
  CPU/GPU 对齐仍为 unknown，因此不用于推断跨时钟关键路径或 GPU 利用率。
- 首次新路径采集 ID 2 触及 128 MiB 上限，保留原文件及
  `direct-capture-byte-limit.json`，不用于此表；ID 3 提高到 256 MiB 后完整重采。

新路径与原设置保持运行，ring/detail/streaming 均关闭。后续应先压缩同步翻译中的
重复工作，并为 Flush 建立实际待写回范围及新旧 CPU 数据的关系，再进一步拆锁；
不要重新引入整队 Finish，也不要用扩大槽数量掩盖未证明的引用寿命。

## 回归与限制

- `MTL_DEBUG_LAYER=1` 的 GPU probe 通过：384 次提交，126 draws，10 dispatches，
  576 个 storage words，以及已有纹理别名、反馈、mask、深度、字体与生命周期像素测试。
- 新回归用受控 fence gate 验证：无关 Invalidate 能推进；相交时间戳在 fence 放行前
  阻塞，放行后内容可见；两个 NVN 槽被标记忙时，呈现准备仍能完成；下一批翻译与
  源纹理 Finalize 可以在呈现复制等待时推进，2048 个复制像素仍正确。
- 自适应与强制逐批两种路径的 GPU 写回链测试均通过。保留第一版测试失败日志：
  原测试要求至少 8 次实际拆批，与允许延后拆批的新策略不匹配；现分别核验强制路径的
  跨槽覆盖和自适应路径的提交/延后覆盖，并保持像素断言。
- Python profiler 测试 8/8、Python 编译和差异检查通过。新的调度非法参数在实际
  debugbus 上验证拒绝，证据为 `schedule-invalid-inputs.json`。
- 本轮编译并运行的是 Mac。Android 共用实现和控制入口已更新，但未重新构建或安装 APK。
- depth bounds 在当前 Mac GPU 上未覆盖；没有用合成像素测试替代实际场景视觉确认。
  用户在当前 PID 21303 版本确认复苏神庙画面正常。
