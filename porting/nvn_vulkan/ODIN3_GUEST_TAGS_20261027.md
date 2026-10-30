# Odin3：BOTW Guest 阶段标签与首轮归因

## 本轮结论

已把 BOTW 1.6.0 的 12 个经过身份、入口指纹和寄存器 ABI 校验的 Guest 函数接入 Litep。Mac Metal、Mac Vulkan、Android Vulkan 共用标签运行时；Android 仅增加启动属性传输。不是在 Host 上给函数名贴别名，也没有重写游戏逻辑。

Odin3 自动进入同一存档后，10 个入口实际命中，drawDRC / postDrawDRC 未命中；`mismatch=0`、`overflow=0`。默认不安装、不记录；本次进程为验证而安装，验证结束后已关闭所有采样并恢复启动属性，游戏保持运行。

新增信息已经把下一步范围显著缩小：

- 主线程 `postCalc_` 平均 **40.63 ms/调用**，其中 7 次 WorkerMgr::sync 合计 **20.97 ms/调用**，另一个直接子级 SVC 0x34 地址等待 **18.16 ms/调用**。两项在同一线程中不嵌套；不是把所有线程等待相加。
- WorkerMgr::sync 并不平均慢。`main+0x1507e78` 调用、返回位置 `main+0x1507e7c`，平均 **19.02 ms**，占该主线程全部 WorkerMgr::sync 累计时间约 **77%**。当前 1.6.0 反汇编确认此处在 postCalc_ 末段调用 `main+0x13a03d0`。
- `invokeCalcFrame_` 平均仅 **0.174 ms**，RootTaskCalc 平均 **8.30 ms**。不应再用一个笼统的 Guest 帧耗时去指认这些函数都很慢。
- 另一线程 TID 25146 的 `NVN.FrontLock.Wait` 仍累计 **1185.87 ms/该窗口**，在 580 次 Flush 中出现 85 次锁等待；翻译线程 TID 25057 的 58 次 `NVN.Translate` 平均 **28.36 ms**。目前没有 Guest 任务执行/唤醒 token 把该 Flush 线程与主线程等待闭合，不能仅凭耗时接近宣称同一条因果链。

这些都是**包含阻塞和抢占的 elapsed 区间**，不是纯 CPU 执行时间。该窗口没有调度侧证据，也不具备可信的 GPU/CPU 时钟对齐，不能据此宣称 CPU/GPU 已满载。

## 精确身份

- 设备：AYN Odin3，ADB `6edb0593`；未操作另一台 Thor。
- 包名：`org.macnative.android.botw`；最终验证进程 `:game` PID **24864**。
- 最终 APK SHA-256：`3005bd46c7a01a0a5d1bb5f912fd5324906dee43750683e608d2b581b54ba62f`。
- BOTW main Build ID：`8e9978d50bdd20b4c8395a106c27ffde00000000000000000000000000000000`。
- main NSO SHA-256：`15cfca7b89348956f85d945fade2e215a6af5991ed1071e181f97ca72f7ae20b`。
- 原始 Android 重写镜像 SHA-256：`cf5d57a21cd85a627de7c9c500957d971b81b6aebbc3360ed78edbe350b41884`。标签在身份校验、现有补丁与重定位后，仅修改私有运行映射。
- 驱动维持 Turnip `e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`。
- 手持 720p、internal scale 1x、realtime 开启、共享 Vulkan 配置、PSO workers=4、ordered translation=0、live range Flush=1。

自动读档验证要求存档已打开、持续呈现、draw>=1000、有 dispatch、无新建 PSO/shader。无控制器注入和截图循环。工作负载门槛证明已进入稳定游戏负载，不单独证明地图名称、相机或视觉正确性；同存档、未移动作为本次对照条件。

## 实测开销

以成功 native present 计数差除以采样时间，先排除加载阶段；不采用状态层某一个瞬时 FPS。

| 配置 | PID | 测量时长 | FPS |
|---|---:|---:|---:|
| 新功能未安装，录制关闭 | 23154 | 15.25 s | 21.052 |
| 已安装但关闭标签与录制 | 24864 | 30.45 s | 21.018 |
| A：共同基础录制开启，标签关闭 | 24864 | 30.37 s | 18.077 |
| B：共同基础录制开启，标签开启 | 24864 | 30.53 s | 17.948 |
| A：共同基础录制开启，标签关闭 | 24864 | 30.44 s | 18.330 |

两个 A 按总帧数/总时长合并为 **18.204 FPS**，B 相对下降 **1.40%**。单轮实验且短窗口有波动，不能把 1.40% 当成稳定精确常数。未安装/已安装关闭的基线相近，也不是严格跨进程显著性结论。

现有共同基础录制相对未录制约下降 13.4%，比新增标签成本明显。性能结论使用未录制 cadence，Litep 用于局部归因；不把录制中的低 FPS 当作发布版本性能。每个 A/B/A 阶段之后另取 3 秒文件，用于观察结构，未把文件捕获过程混入上述 cadence。

## Guest 到 Host 的结构证据

主采样 `ab2/on/file-1793094635182840-2.prof`：SHA-256 `a61a17b975e50901e127b5ca1c241c6f57ba6ff6441a1eb73ebd69f01bc1f7ab`，观测 2.969 s，59 个 native present marker、58 个完整 marker 区间。

| TID 24991 区间 | 完成次数 | 平均 inclusive | 备注 |
|---|---:|---:|---|
| postCalc_ | 58 | 40.626 ms | 不能与下列子项相加 |
| RootTaskCalc | 58 | 8.303 ms | 包含内部计算和等待 |
| WorkerMgr::sync | 870 | 1.645 ms | 15 次/约一轮，存在明显 top |
| invokeCalcFrame_ | 58 | 0.174 ms | 本窗口在 WorkerMgr::sync 下执行 |
| drawTV_ | 59 | 0.263 ms | 函数区间，不是 GPU draw 总耗时 |
| updateGPU_ | 59 | 0.199 ms | 包含 590 次 invalidate 调用 |

通过 Litep 原生 `zone_sequence / parent_sequence` 同线程关系，确认 postCalc_ 下 406 次 sync 和 58 次 SVC 0x34；不按事件数组位置配对。通过每次调用中的 `Guest.Tag.CallerOffset`，15 组 sync 调用点中最慢一组稳定落在返回位置 `0x1507e7c`。

队列发布记录也已解析出 25 个实际任务名位置，包括 `ModelScene::calc`、`SystemTask::calcFrame`、`renderer-0`、`renderer-2`、`ParallelGsysCalcModelQueueJob`、`ksys::MessageDispatcher` 等。原始记录只存名称指针的模块偏移；离线从 SHA 匹配的镜像读取。发布队列尚未绑定到工作线程执行起止，因此这些名字不能直接充当各 Worker 的耗时归属。

主采样 container/payload 完整、295 chunks 正常、0 skipped、0 pairing error，无内部录制代际断点。保留 44 个捕获起点 unmatched end、42 个终点 open span；不把这些边界当成函数错误。GPU 有 unknown-context / incomplete-zone 诊断，GPU/CPU alignment unknown；本轮不以 GPU 数据推导关键路径。

另做 HLE 与 Guest 标签同时启用的 1 秒兼容性检查，未产生 `HLE.Import.GuestTag.*` 交叉区间，运行时 mismatch/overflow 仍为 0。但 SDK 报告 **2 个 dropped chunk marker**；该附加文件只保留为兼容性诊断，排除出耗时和性能结论。常规采集保持 HLE 全量模式关闭。

## 实现与验证

- `porting/profiler/guest_tags.*`：默认关闭的入口适配器、代际检查、有限深度调用栈、调用 ID 和上下文计数。
- `games/botw/guest_patches/performance-tags/`：12 个 SHA 固定的重建符号和 ABI；native-debugger v2 index 验证通过，精确 NSO 源 SHA 校验通过。
- `runtime_hle_wrap_profile_marker`：复用已有宿主栈/TLS 桥接，同时排除 begin/end 桥接自身的 HLE 计时，保持 scope 嵌套。
- Mac/Android 加载器：单独代码页、全入口校验后安装、RX 封页、指令缓存失效；实时帧率数据页保持原有权限。
- `profiler_tags status/start/stop`：共用 debugbus；Android 测试脚本检查精确 PID 和安装回执，busy 状态有界重试。
- Mac Metal、Mac Vulkan、Android APK 与 Android lint 构建通过。Mac/Android 的新 ARM64 探针、既有 Android HLE ABI 探针、共享调度与目录一致性检查通过。新探针覆盖错误身份/指纹时整组不修改、12 个汇编入口、嵌套、整数/浮点参数返回、x8 间接返回及录制切换。

Mac 在本轮完成编译、入口/ABI 探针验证，未做 Mac 游戏场景采集；实际游戏命中和数据来自 Odin3。

## 下一步

优先围绕 `main+0x1507e78` 的那组 WorkerMgr 同步继续建立 producer/consumer 关系，补任务出队、执行与完成 token；并将 postCalc_ 的 SVC 0x34 等待地址、对应唤醒者与 TID 25146 的 Flush 串联。当前证据能证明存在明确 top，尚不能证明哪个任务或 Flush 导致该等待。

等这条关系闭合，再决定范围依赖和 Flush 全局锁应该如何拆解；先避免平均摊薄全部 Draw/Guest 耗时，也不因为 main 线程在等待就直接修改游戏的同步语义。

## 产物与恢复

证据根目录：`/Users/bytedance/workspace/native-switch-study/android-translate-20261026/guest-tags-20261027/`。`ab2/result.json` 保存开销对照与实际命中，`litep/queries.json`、`litep/attribution.json`、原始 PROF 和持久索引保留关联依据。

构建日志、APK、探针和本轮独立 diff 在 `android/build/guest-tags-20261027/`。最终 APK 相对上轮仅改变 native library（签名元数据之外）。没有提交、上传私有模块、存档或密钥。

安装前后保留的 78 个设置/缓存/存档文件无增删；最终 77 个 SHA 不变，仅游戏正常写入的 `option.sav` 变化，未回滚它。没有清空数据、卸载应用、修改频率或使用 root。启动属性已恢复，所有采集模式已关闭，PID 24864 保持游戏运行。
