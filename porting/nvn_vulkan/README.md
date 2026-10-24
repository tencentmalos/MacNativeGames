# BOTW 原生内存与 NVN→Vulkan 接入

本阶段只推进 BOTW 1.6.0。Android APK 为 `0.4-memory-vulkan`，仍是运行时预览。
**已经验证真实 GPU 的清屏、draw、dispatch 和读回；尚未运行 BOTW 游戏画面。**

## 实现与参考边界

| 部分 | 当前实现 | 参考与限制 |
|---|---|---|
| 内存 | `../runtime/guest_memory.*`：39 位内真实预留，堆提交/释放，物理页映射，栈移动与写回，权限和统一查询 | 对照 Mac `services.c` 的原生策略及 Citron 的 SVC ABI/错误定义，自行实现宿主映射；不链接模拟器内存管理器 |
| NVN 前端 | `nvn_vulkan.cpp`，通过函数名查找并在编译期校验原型；当前注册 113 个入口 | 对照 Citron `frontend_nvn` 的录制、对象及完成语义，和 Mac/D2R 原生实现。注册数量不是 BOTW 完整覆盖率 |
| Vulkan | 固定 D2R Mobile `758f5d41509af5cd38b7eb934bc2334bdb3e3342` 的独立引擎 | 直接编译只读源码；不链接 D2R 宿主、游戏补丁、音频或 shader。无需复制 Citron GPU 通道、SMMU 或 Maxwell 回放器 |
| Shader | XGS 离线 SPIR-V 与反射格式，按 shader control 摘要查找，宿主驱动建立 Vulkan 管线 | 合成测试使用本目录内手写 GLSL；不是 BOTW shader。Mac metallib、D2R 游戏 shader 和不同游戏的缓存不能代用 |
| 平台设施 | 保留 Foundation 音频与手柄，APK 增加独立 GPU 验证入口 | 不在 UI 线程运行 GPU 测试；未接入游戏窗口、完整生命周期或 guest 图形调用桥 |

不透明 NVN 对象只写入 8 字节宿主句柄，完整对象由宿主管理；避免把 D2R 大结构写入
Zelda 较小的对象存储。按值复制的纹理视图使用 16 字节本地表示，再转换为引擎视图。
实测分别用保护字节检查对象边界和视图复制。这里定义的是适配器自己的对象表示，
没有把其他游戏的指令地址或字段补丁写入 BOTW。

命令使用 NVN 录制语义进入 Vulkan 引擎。`CopyCommands` 在录制时复制记录，
`CallCommands` 保留引用；纹理和 program 的录制快照保持稳定身份。同步结果只依据
GPU 完成状态。当前 `QueueFlush` 保守地等待完成，因此它仍有明显优化空间。
对象、命令段和快照保留到会话结束并有容量上限；尚不适合长时间游戏的资源回收策略。

## 内存服务

已接入独立启动探针的 SVC：

- `SetHeapSize`、`SetMemoryPermission`。
- `MapMemory` / `UnmapMemory`：目标限定为栈区域，源限定为已提交的普通堆/别名区；
  映射期间源页标记锁定并撤销访问，撤销时写回。当前只接受匹配的完整映射撤销。
- `QueryMemory`、`MapPhysicalMemory` / `UnmapPhysicalMemory`。
- `GetInfo` 的堆、别名区、栈、ASLR、内存池及已用量；未处理的信息 ID 继续明确停止。
- 启动主线程的优先级、核心掩码、线程 ID 查询；不声称已经实现线程创建或调度器。

堆与物理映射共享同一资源预算。重叠映射保留已有数据；释放后重新映射为零。
宿主 16 KiB 页包含多个 guest 4 KiB 页时，物理权限采用有效子页权限的并集，
`QueryMemory` 保留精确的 guest 权限。因此在 16 KiB 宿主上，不能用硬件保证同一
宿主页内某个 4 KiB 子页单独触发保护故障。这是显式限制，不是与 Horizon 完全等价的声明。

`SetMemoryAttribute`、共享/传输内存等未实现路径不返回伪造成功。当前堆容量和资源池
沿用原生宿主策略；统计是 guest 逻辑预算，不能当成 Android RSS 或系统可用内存。

## 验证方式

按 `android/dependencies.json` 准备 Foundation、Oboe、D2R 三个精确检出。
Android CMake 生成后可分别构建 `mng_memory_probe`、`mng_vulkan_probe`、
`mng_startup_probe`。独立工具示例：

```sh
mng_vulkan_probe --list-procs
mng_vulkan_probe /private/writable/nvn-test/shaders
mng_memory_probe
mng_startup_probe /private/prepared/botw160 botw160
```

GPU 测试只在传入的私有目录下创建自有临时 shader 文件，完成后移除自有临时目录；
不读取或修改 ROM、密钥、存档和游戏 shader。手写测试源为 `tests/triangle.vert`、
`tests/green.frag`、`tests/storage.comp`，生成的 SPIR-V 位于 `tests/synthetic_spirv.h`。
生成命令为 `glslangValidator -V --target-env vulkan1.1`，输出经过 `spirv-val` 验证。

Odin3 / Adreno 830 上的验证包含：

1. 两次清屏提交、2048 个 RGBA 像素逐项检查、128 个裁剪区域像素检查。
2. 修改原命令存储后，已复制的命令仍正确执行。
3. 2 个离线图形 shader、1 个全屏三角形 draw，逐像素验证绿色结果。
4. 1 个离线 compute shader、1 次 dispatch，64 个 storage buffer 输出字逐项检查并写回宿主内存。
5. fence、纹理读回、对象保护字节、16 字节视图复制，以及连续会话初始化/关闭。

这证明的是适配器和 Vulkan 的基础数据通路，不包括 BOTW shader 的反射正确性、
全套纹理格式、渲染状态、虚拟内存池、采样器、事件、窗口呈现或实际游戏性能。

## 下一处集成边界

BOTW 原始 SDK 已通过 16 次启动服务调用，返回首个 loader 通知，停在 SVC `0x1f`
的服务管理器连接。对应 SDK Build ID 为
`fcf93a41b08c5193a1410d262602dc621617c56f`，观察到的 SVC 模块偏移为 `0x37eb50`。
地址只用于本次证据定位，不作为跨版本补丁。

后续需要先连接服务管理器 IPC、线程与构造函数，再把 NVN bootstrap / proc-address
接到维护 Android TLS、x18 和宿主栈的 guest 调用桥。当前 GPU 验证入口调用的是宿主
函数表，未把这 113 个入口直接暴露给原始 guest。继续按实际调用补齐 NVN 能力，
准备 BOTW 专用 XGS，再实现 Android Surface 呈现。未注册接口返回空指针，
已注册接口遇到不支持的参数会明确终止，不能用成功桩绕过。

没有启用任何新的 BOTW guest patch，没有推进 TOTK，没有实际游戏帧率结论。
