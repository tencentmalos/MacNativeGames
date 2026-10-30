# 渲染录制与重放

这套设施用于固定一个错误现场，反复检查 draw、shader、资源和最终输出。
第一版的可执行重放载荷是 RenderDoc RDC，NVN 索引负责把宿主 GPU 事件连接回原生命令消费点。
它不需要再次运行游戏 CPU，但也不会重新执行修改后的 NVN→Vulkan 转换代码。
验证转换代码修复仍需重新录制；不能把旧 RDC 重放成功写成新转换代码已通过。

## 组成与边界

| 部分 | 职责 |
|---|---|
| `runtime/render_record.*` | 与平台无关的录制窗口、连续 Present、事件/字节/时间上限、完整性终止记录 |
| `nvn_vulkan/render_record_vulkan.*` | 显式启用的 Vulkan marker 后端；当前对接 Pico RenderDoc 层 |
| `nvn_vulkan/render_record_engine.inc` | 在已验证的 NVN 命令消费点生成索引，记录提交、opcode、参数摘要、纹理布局及 shader family |
| `tools/render_record.py` | Android 环境备份/恢复、录制触发、采集、哈希验证、真实 MCP 重放和标记校验 |
| Foundation `gpu-snapshot` | 已有的通用证据容器，打包 RDC、NVN 索引和 manifest |

Android 的 Debug APK 默认不启用此设施。启用后仍需显式请求，才会从下一个成功 Present
开始录制 1–8 个完整窗口。默认 NVN 索引上限为 64 MiB、250,000 条事件、60 秒；
字节参数只约束 NVN 索引，RDC 的资源初态可能更大，采集器另有默认 2 GiB 拉取上限。
超限、取消、失去连续 Present 或文件错误保留不完整证据，不报告成功。
进程崩溃留下没有 footer 的文件，验证器会拒绝它。

每次运行生成随机 run ID，每个请求具有 capture ID。GPU 点标记包括
`run/capture/frame/submission/event/op`，不能仅凭文件名、时间或相同计数器配对。
一次 NVN 操作可能对应多个 GPU 标记；纯状态命令也可能没有 GPU 标记。
`command_address` 是翻译后的宿主命令存储地址，不是 Guest PC。
Guest 调用者和完整 Guest→资源区间溯源仍为 unknown，不能由该地址推断。

公共窗口控制器可供其他平台后端使用，但当前接入和实机验证仅覆盖 Android Vulkan。
Metal 捕获、可跨后端重新翻译的独立 NVN 文件、完整资源别名与 CPU writer 追踪尚未实现。

## Android 录制

需要已经安装的、与宿主重放库配对的 Pico RenderDoc Server，及当前 Debug APK。
工具不会安装/卸载包、清除数据、修改存档或自动选择游戏。所有输出放在 Git 外私有目录。
以下命令在仓库根目录执行，`CASE` 必须是新目录，`SERIAL` 必须明确指定。

```sh
python3 porting/tools/render_record.py --adb /path/to/adb prepare CASE \
  --serial SERIAL --package org.macnative.android.botw \
  --process org.macnative.android.botw:game --profile native-botw160
```

随后正常重启目标应用并进入待验证的场景。`prepare` 在写入任何设置前保存恢复清单。
应用日志的 `[render-record] ready` 给出本次运行的私有目录。

```sh
python3 porting/tools/render_record.py --adb /path/to/adb arm CASE --frames 1 --trace-mib 64
python3 porting/tools/render_record.py --adb /path/to/adb collect CASE
python3 porting/tools/render_record.py --adb /path/to/adb restore CASE
python3 porting/tools/render_record.py verify CASE
```

`arm` 核对单一目标 PID、已加载的 RenderDoc 映射、APK 哈希和系统 fingerprint。
`collect` 必须找到唯一新 RDC，并核对设备与本地哈希；索引必须有完整 footer 和连续事件。
如果太早采集，失败的索引尝试单独保存，可待录制完成后重试。
`restore` 可在失败后独立执行；已加载的 Vulkan 层要等该进程退出才卸载。
`verify` 只验证包完整性，首次采集后 `replay_verified` 保持 false。

## 同设备重放

停止游戏进程释放 GPU 资源后执行。显式传入同一发行版的 MCP server、原生库和 ABI manifest，
不能把 Android Adreno RDC 交给本机 macOS GPU 重放。

```sh
python3 porting/tools/render_record.py --adb /path/to/adb replay CASE \
  --output NEW_REPLAY_DIRECTORY \
  --mcp-server /paired/RenderDoc.Mcp.Server \
  --library /paired/librenderdoc.dylib \
  --abi /paired/osx-arm64-pico.json \
  --replay-marker /paired/RenderDoc.ReplayMarker.dylib
```

重放前再次检查设备、Server APK 和 RDC 哈希。工具调用真实 MCP 的 `tools/list`、
`capture_open`、`capture_status`、`frame_overview`、`schema_describe` 和事件查询，
逐条核对 RDC 中的 run/capture 标记及 NVN event/frame/submission/op。
没有匹配标记时明确失败，不以有截图或能打开文件替代配对证明。

输出包括 MCP 原始结果、`replay.json`、`nvn-marker-map.json` 和清理结果。
增加 `--event EID --resource RESOURCE_ID --mip 0 --slice 0` 可通过 `review_texture`
导出并保存图像内容。图像是否正确仍需实际查看；中间纹理不能自动视作最终显示。
结束时关闭 replay controller，并恢复此次操作之前的 Server 配置、转发和运行状态。

## 打包与对照

```sh
python3 porting/tools/render_record.py pack CASE \
  --foundation /path/to/foundation --output NEW_CASE.gpusnapshot
```

容器复用 `foundation.gpu-snapshot.v1`，保持 Foundation 对完整性与语义正确性的区分。
容器中的哈希验证不自动提升为同帧或画面正确证明。

BOTW 对照采用 Citron NVN、Citron Maxwell 和 Android 原生 NVN 三条路径：

1. 核对同一 Program ID、主模块 Build ID、存档来源及实际场景；分别记录每条路径的补丁、构建和驱动身份。
2. 先检查上游输入：高度/材质纹理、mip/layer、布局、UBO、顶点和索引。
3. 再比较对应 draw 的 shader、viewport、深度、剔除及输出；跨实现不能直接比较 RenderDoc resource/event 数字。
4. 通过 shader 身份、输入内容及用途建立对应关系，并保留不确定项；定位首个可证明的差异。
5. 修改后重新录制相同场景，检查该差异和最终输出。性能测量另用未启用捕获层的运行。

Citron 的两条路径是需要核验的对照证据。某条路径能启动或进入场景，不代表其每张中间纹理都正确；
两条路径一致及原始 NVN 语义可相互印证，也需要留意共享 Vulkan 后端导致的共同错误。

## 自动检查

宿主 CTest 的 `mng_render_record_probe` 覆盖连续窗口、重复请求、事件/字节/时间上限、
后端不可用、取消及旧文件保留。Python 测试覆盖截断、重复事件、帧错位、错误身份、
哈希篡改及路径越界：

```sh
python3 -m unittest discover -s porting/tests -p test_render_record.py
```

合成测试只验证协议和控制逻辑；实际 replay 成功必须来自设备上的真实捕获，不能用测试 fixture 代替。

## 自定义 Turnip 重放

`arm` 保存完整进程映射、实际映射的 Turnip ELF 哈希及 driver-status。
若捕获使用 Turnip，`replay` 必须传 `--android-vulkan-loader`，指向配对 Server 私有
files 目录中的 `librenderdoc_turnip.so`；相邻 `vulkan.ad07xx.so` 的哈希须与捕获一致。
需一并部署 adapter 的四个 hook 库，并记录其哈希。工具备份/恢复
`debug.rdoc.vulkan_loader`，通过重启 Server 应用该选择，不静默回退到系统驱动。
实际 driver ID / driverInfo 仍需从本次 replay 日志核验；文件一致只是其中一项证据。
