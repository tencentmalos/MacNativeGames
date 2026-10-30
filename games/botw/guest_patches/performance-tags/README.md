# BOTW 1.6.0 Guest 性能标签

本目录保存逐游戏标签的可校验语义目录；通用运行时在 `porting/profiler/guest_tags.*`，Mac Metal、Mac Vulkan 与 Android Vulkan 共用。这里只增加计时，不替换游戏函数实现，不修改磁盘上的游戏模块。

## 身份和覆盖

目标为 Switch BOTW 1.6.0，Program ID `01007ef00011e000`，main Build ID `8e9978d50bdd20b4c8395a106c27ffde`（NSO 字段补零至 32 字节）。完整 NSO SHA、分析 ELF SHA、Android 重写 SHA 与入口指纹在 `guards.json`。加载器先验证完整重写镜像，标签安装再次验证源身份及全部 12 个入口的 16 字节指纹；任一失败即拒绝整组安装。

`index.json` 是 SHA 固定的 `spatial.guest-symbol-index.v2`，`botw160.symbols.json` 保存模块相对偏移、函数范围、寄存器 ABI、语义来源和置信度。名称均为 **correlated 重建语义**，不是原厂导出符号；已查询当前模块的 MOD0 动态符号表，这些入口未导出。类型保持不透明，不从其他版本移植类布局。

| 标签 | 用途 |
|---|---|
| RootTaskCalc | 根任务计算；包含其内部状态机，不能全部算作纯游戏逻辑 |
| preCalc / postCalc | 图形计算准备和阶段调度 |
| drawTV / drawDRC | 绘制阶段；命中不代表第二块屏幕实际输出 |
| postDrawTV / postDrawDRC | 绘制后处理阶段 |
| invokeCalcFrame / updateGPU | 图形帧计算与 GPU 更新调用 |
| WorkerMgr pushJobQueue / run / sync | 队列发布、启动和同步等待 |

旧校准 CSV 的 preCalc、postCalc、drawTV 三个硬编码入口指纹与当前原始 ELF、Android 重写镜像均不符，已拒绝旧指纹，按当前镜像重新生成。原有别名还把 postCalc 称为 invokeCalcFrame、把 drawTV 称为 updateGPU；本目录按当前字符串、调用关系和委托证据纠正，保留偏移供交叉检查。不会把其他版本的地址直接用于安装。

## 安装与采集

默认不安装；没有启动开关时，Guest 代码不增加分支。开发 APK 通过 Android 属性 `debug.mng.guest_tags=1` 传递统一的 `MNG_GUEST_TAGS=1`，Mac 直接使用该环境变量。它是 Guest 诊断设置，独立于图形后端策略。

安装在启动期间、Guest 线程尚未执行时完成：为标签单独分配代码页；原有实时帧率数据页继续可写，标签页单独封为 RX。每个入口仅替换一条已证明不依赖 PC 的指令，经近跳板转到宿主汇编；原始指令与后续函数体仍执行。转发只支持目录中核实的寄存器传参 ABI，保留整数、浮点和间接返回值，不使用 x18。需要栈传参的未知函数必须先扩展适配器和测试，不能直接加入目录。

安装后仍默认不记录。使用 debugbus：

```text
profiler_tags status
profiler_tags start
profiler_capture file 256 3
profiler_capture status
profiler_tags stop
```

Mac 使用同一命令注册表。Android 可通过 `porting/tools/profiler.py` 发送命令并按 PID、capture ID 和 SHA 收集文件。复现脚本：

```sh
python3 -I android/tools/benchmark_guest_tags.py --serial SERIAL --expected-pid PID --output NEW_DIRECTORY --seconds 30 --capture-seconds 3
```

脚本要求已验证进入场景、无其他采集、标签已安装；保留当前配置，测量关闭录制、共同基础录制下的关/开/关，并分别保留 Litep 文件。启动和自动读档使用 `android/tools/validate_scene.py`，不把加载界面计入帧率。不能从工作负载门槛推断具体地图、相机位置或视觉正确性。

## 时间线语义和限制

每次调用带 `Guest.Tag.Invocation / Parent / Epoch / Owner / Module / Offset / CallerModule / CallerOffset / Stack / Object`，WorkerMgr 发布额外带队列、任务名的模块偏移、核心掩码和同步类型。名字只保存地址偏移，不在热路径解引用 Guest 字符串。调用 ID 是整数计数，当前 native 运行时每个 Guest 线程对应一个 pthread；本实现未支持协程迁移到其他物理线程。

这些标签是**函数调用区间**，不是完整 Guest frame 或 Present token。同线程嵌套可用于把 NVN Flush、锁等待、Guest 条件等待归入对应阶段；不能仅凭相邻时间推断跨线程因果。`pushJobQueue` 的队列地址也不等于任务已经在某个 Worker 上开始执行。没有标签的 Worker 子任务仍属未知，需要后续语义还原。

区间包含睡眠、抢占和 Host 调用，不是 on-CPU 时间；inclusive 和嵌套耗时不可相加。CPU/GPU 总瓶颈需要结合现有 Host NVN、提交/呈现标记以及有证据的调度数据。录制切换会使跨边界调用被放弃，`abandoned / stale_returns` 明确显示；`mismatch / overflow` 应为 0。Litep 文件自身的首尾截断、丢块、未匹配事件也必须保留，状态计数不能代替文件完整性检查。

Begin/end 桥接通过现有 HLE 机制切到宿主栈，但明确不再套 HLE 计时区间，避免 scope 交叉关闭。其他真实 Host 调用仍保留原有标签。

## 验证

`porting/tools/check_guest_tags.py` 检查符号目录 SHA、ID/偏移和编译目录一致性；可用 `--private-image` 校验本地原始重写镜像。`mng_guest_tags_probe PRIVATE_IMAGE` 在 Mac 和 Android 测试全部入口的失败原子性、汇编转发、嵌套和录制切换；测试函数不执行游戏指令。私有镜像只在外部工作目录使用，不进入仓库。
