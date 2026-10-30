# 预编译 Shader 连续归档

跑图时首次绑定 Shader，原先会在同步翻译锁内分别打开 XGS/XGV 文件。已有 Pipeline
预热仍不能消除这些文件读取。本实现只改变预编译数据的存储与读取方式，不重新翻译
Guest Shader，不改变 SPIR-V、variant 选择、驱动编译或资源同步规则。

```sh
python3 porting/tools/shader_archive.py /private/shaders /private/shaders-v1.mngsa
python3 macos/run.py ... --backend vulkan --shaders /private/shaders \
  --shader-archive /private/shaders-v1.mngsa
```

底座也接受 `MNG_SHADER_ARCHIVE` 绝对路径，供 Android 宿主复用；本轮未接入 Android
启动器 UI 或 APK 资产发布。未指定归档时保留散文件路径。指定后它是该进程的权威数据源，
缺少 XGS 会报错，不混用另一份散文件；缺少可选 XGV 按原逻辑选择单 Shader。原诊断
`spv-override` 仍生效。每份归档必须绑定所选游戏、版本、转换器和 variant 数据；
不能把别的 Shader 包按同名文件替换。

格式是小端、只读、无压缩的 `MNGSHD1` v1：32 B header，40 B 定长索引，随后是原始文件
内容。条目名是 16 个小写十六进制字符加 `.xgs`/`.xgv`。读取器核验版本、总长、条目
数量、排序、重复、名字终止、范围溢出和负载重叠。一次 mmap 后按名字二分查找，结果
仍进入原来的 XGS/XGV 语义校验。索引和映射只读，可由预热与渲染线程共用。

写入工具拒绝覆盖已有归档，使用独立 `.partial` 文件完成后原子发布，并输出每项
SHA-256 和总文件 SHA 的 sidecar。不要修改正被进程映射的文件；更新时生成新名字。
Shader 内容、sidecar 和游戏归档保留在私有资产目录，不进入源码仓库。

基础 Litep 增加 `NVN.Shader.Load`（缓存 miss 全过程）、`NVN.Shader.Read`（文件或
归档读取）和 `NVN.Shader.Key64`。Key64 是 16 位十六进制文件名的无符号 64 位值，
分析工具显示负整数时按模 2^64 还原。它是 Shader 身份，不是 Guest 地址或帧 token。
不要把 `Host.Pipeline.Compile` 的时间当作完整 Shader 首次加载时间。

`python3 -m unittest porting.tests.test_shader_archive` 包含 C++ 实际读取器的字节往返、
缺失项、截断、溢出、重复名字、坏名字、负载重叠，以及不覆盖原文件等 9 项测试。
回归还必须用同一份合成 Shader 的散文件和归档分别执行 `mng_vulkan_probe`。
