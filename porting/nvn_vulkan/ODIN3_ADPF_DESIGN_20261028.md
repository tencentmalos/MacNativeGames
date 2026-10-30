# S5：宿主工作时长提示实验

O2 版的两次独立调度采集显示：R 翻译区间主要在 CPU 上执行，宿主 runnable
等待很小；实际频率约 2.6 GHz。设备的 `performance_hint` 服务报告 HAL 可用、
建议更新周期 16,666,666 ns。因此先测 ADPF，尚不以未证明的 M/R 双向锁步进入 S2。
这只是实验依据，不证明提高频率必然提高整帧吞吐。

## 结构与时长口径

- 共享 Vulkan 在翻译末尾提交已有的批次 elapsed；当前场景每个完整源帧有一批翻译。
- 呈现只测 Acquire 返回后到 QueuePresent 前的命令准备与提交，排除等交换链、
  VSync 和复制 fence。两类周期工作各有一个 Hint Session，预算均为 60 Hz 的
  16,666,667 ns。不是把两个线程的时间相加，也不把 GPU 时间当作 CPU 工作。
- 一个宿主 worker 独占 Android Hint API。生产者只更新每通道一个有界样本槽；
  worker 取出后释放槽位锁，再调用 Android 服务。Binder 不在 front/engine 锁上执行。
- Session 只使用本进程实际提交样本的 TID。TID 改变时关闭旧 Session 并重新创建；
  失败后不逐帧重试，需 OFF/ON 或新的 TID。退出关闭所有自己创建的 Session。
- `renderer_hint on|off|status` 默认 OFF；generation/applied 区分请求与生效，
  tid、accepted/reported/coalesced/failures 区分真实工作和失败。OFF 后要等 applied
  追上且两个 tid 为 0，才开始对照窗口。
- Android 只在平台适配器内动态解析公开 NDK API；Mac 明确报告 unsupported。
  Vulkan 命令、缓存、依赖、Flush 和 Guest 调度仍为同一份代码。

## 验证

先验证工作线程独占、两通道独立 Session、TID 变化、阻塞后端下生产者/关闭请求
仍可返回、有界合并、失败不重试、OFF 和析构关闭；再做双平台 GPU probe。
实机同 PID OFF/ON/OFF/ON，保持 720p 手持、1×、publication/invalidate=1、
ordered=0、realtime=1、PSO=4、显示60Hz。每窗记录温度、Thermal Status、native
Present；随后取独立 Litep/Perfetto/SF。至少一段 ON 持续300秒。
FPS +2% 或 p99 改善10%且无热状态恶化才接受；无收益则保持默认关闭。
不修改频率、亲和性、Guest 核数、核掩码、优先级或同步语义。
