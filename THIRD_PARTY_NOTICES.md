# Third-party code

Code in this repository that comes from other projects keeps its license; the license file sits next to the code.

| Code | From | License | File |
|---|---|---|---|
| `games/totk/third_party/d2r` (NSO rewriter, verification tools) | [d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile), adapted | MIT | `games/totk/third_party/d2r/LICENSE` |
| `games/totk/third_party/d2r_gfx` (NVN on Metal, texture tools, host headers) | d2r-mobile, extended for this game | MIT | `games/totk/third_party/d2r_gfx/LICENSE` |
| texture layout sizes in `games/totk/third_party/d2r_gfx/gfx/nvn/nvn_tex.c` | deko3d | zlib | `games/totk/third_party/d2r_gfx/LICENSE.deko3d` |
| `games/totk/third_party/d2r_gfx/third_party/bcdec` | [bcdec](https://github.com/iOrange/bcdec) 0.985 | MIT or public domain | `games/totk/third_party/d2r_gfx/third_party/bcdec/LICENSE` |
| `games/totk/tools/totk_xlate` (shader translator front end) | d2r-mobile, adapted | MIT | `games/totk/tools/totk_xlate/LICENSE` |
| `vendor/ryujinx` (Ryujinx.Graphics.Shader, Ryujinx.Common, Spv.Generator) | Ryujinx, as pinned by d2r-mobile; patched by `games/totk/tools/totk_xlate/patches` | MIT | `vendor/ryujinx/LICENSE.txt` |
| `games/totk/prebuilt/macos-arm64/totk_xlate` (the shader translator, a native program) | built from `vendor/ryujinx` + the patches + `games/totk/tools/totk_xlate` with `publish_native.sh`; contains .NET runtime libraries compiled in by NativeAOT | MIT (Ryujinx, d2r-mobile, .NET) | `vendor/ryujinx/LICENSE.txt`, `games/totk/tools/totk_xlate/LICENSE` |
| in `games/totk/runtime/threads.c`: SignalToAddress that wakes all waiters subtracts 2, SystemEvent bindings that end with their write handle, signals that wake only the waiters of the signalled object (checked by `games/totk/tests/system_event_bind`) | [nxnative](https://github.com/seedds/nxnative) commits `d47f899`, `49ee623`, `df7badd`, rewritten for this runtime (via PR #159) | MIT | `games/totk/third_party/licenses/nxnative.txt` |
| audio effects in `games/totk/runtime/audio_reverb.c`, `games/totk/runtime/audio_effects.c` (delay, reverb, compressor) and the mixer | ported from Ryujinx.Audio | MIT | `games/totk/third_party/licenses/Ryujinx-Audio.txt` |

Fetched or linked at build time (not in this repository):

| Code | Use | License |
|---|---|---|
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) at `aa217ae` | SPIR-V to Metal Shading Language | Apache-2.0 |
| [hactool](https://github.com/SciresM/hactool) at `1d64a83` (with explicit private title-key-file hook), including its bundled mbedTLS | Base + update BKTR merge at build time only | ISC (hactool), Apache-2.0 (mbedTLS); see fetched source LICENSE files |
| libopus (Homebrew), linked statically | Opus audio | BSD-3-Clause |
| libvpx (Homebrew), linked statically | VP9 movies | BSD-3-Clause |

## Android fork dependencies

The Android work is maintained at `tencentmalos/MacNativeGames`, separately from
upstream's Apple distribution. `android/dependencies.json` pins the external
Foundation and Oboe source checkouts; no dependency source or private game data
is copied into this repository.

- Foundation `modules/audio`: MIT, including its callback mixer and Oboe endpoint.
- Foundation `modules/input`: GPL-2.0-or-later, Copyright 2026 shadPS4 Emulator
  Project; native InputHub and the Android Kotlin collector retain these notices.
  Android bridge/test files using this API are GPL-2.0-or-later. The combined
  Android application includes GPL components; it must not be represented as an
  MIT-only binary. The existing MacNativeGames MIT files retain their notices.
- Oboe: Apache-2.0; use the LICENSE and AUTHORS in the pinned source checkout.
- Android ABI emitter adaptations: `jhaizhou-ops/d2r-mobile` at
  `758f5d41509af5cd38b7eb934bc2334bdb3e3342`, MIT, Copyright 2026 jhaizhou.
  The existing `games/totk/third_party/d2r/LICENSE` carries this notice.
- `android/third_party/d2r/sha256.c`, `sha256.h` and the syntax conversion in
  `android/tools/asm_elf.py`: adapted from D2R Mobile at the same `758f5d4`
  revision, MIT. The SHA-256 subset retains its license beside the source.
  The shared `porting` core also uses that subset; its C symbols are prefixed
  `mng_port_` to avoid colliding with Apple's CommonCrypto in the Mac loader.

The Android study builds are local development artifacts, not a binary release.
Their source dependencies and applicable notices must accompany any distribution.

## Android Vulkan 组件

`android/dependencies.json` 固定 D2R Mobile 的 `758f5d41509af5cd38b7eb934bc2334bdb3e3342`。
只编译 `dm/gfx/vulkan/engine`、`nvn_tex.c`、`nvn_deswizzle.c`；不链接 D2R 宿主、
游戏补丁、音频输出或游戏 shader。NVN 接口适配器位于 `porting/nvn_vulkan`。

许可原文同时打包在 APK 的 `assets/licenses`：D2R MIT、bcdec MIT/公有领域双许可、
deko3d zlib（纹理布局算法）、h1k421 重建 NVN 头文件 MIT。法律许可原文保留英文。
接口语义对照 Citron 的 NVN 前端与测试；没有复制 Citron 的 GPL GPU/内核实现到 MIT 底座。

`porting/nvn_vulkan/prepare_engine.py` 在构建目录生成 D2R 引擎的适配副本：
增加顶部时间戳、纹理 swizzle、depth clamp 与 BOTW 短 shader control 段哈希处理。
原检出保持只读，生成副本仍适用 D2R 的 MIT 许可。`state_api.inc` 的状态打包器
来自本仓库原有 MacNativeGames NVN 实现，保留 MIT 标识；不包含游戏地址或补丁字节。

`attachment_alias_engine.inc` 对齐原 MacNativeGames `nvn_obj.h` 的
`rt_dup_of`；原注释将该重复附件处理归属到
[seedds/nxnative](https://github.com/seedds/nxnative)（MIT）。保留本仓库的
`games/totk/third_party/licenses/nxnative.txt`。实现额外检查有效 format 和重复槽写掩码。

## Android 启动器与状态层依赖

- volk：MIT；外部固定版本见 `android/dependencies.json`，许可随 APK 保存。
- libadrenotools / liblinkernsbypass：BSD-2-Clause，保留各自许可。
- Mesa Turnip：按源码逐文件许可及 Mesa license 文档保留声明。静态链接的
  NDK libc++/libc++abi/unwind 声明随 APK 的 `turnip-NDK-toolchain-NOTICE.txt` 保存。
- Dear ImGui：MIT，官方 1.92.2b 宿主版本，与 Foundation 的状态层 ABI 配套。
- FreeType：使用 FTL 许可选项，保留 `LICENSE.TXT` 与 `FTL.TXT`。
- fmt：MIT，保留原许可。
- Foundation `imgui_overlay`、`perf_metrics` 及其共享实现：源文件声明
  “Copyright (c) 2026 tencentmalos. All rights reserved. For study and research only.
  Not for redistribution.” 它不是 MIT 依赖，当前组合仅用于本地研究，不能公开再分发。
- 背景和图标来自用户已有 EmuPSP Ghost/Nlib 缓存，Program ID `01007EF00011E000`。
  图片权利属于原权利人，仅作为用户本地构建输入，不提交素材文件。

## Foundation PSO preparation jobs

The shared Mac/Android Vulkan backend links the pinned Foundation `basic/async/async_task`, core, reflection, allocator and property targets. The existing standalone profiler ring remains the single SDK owner. Local APK notices include `foundation-jobs.txt`, mimalloc MIT, the Boost Software License for Asio support, and Yalantinglibs Apache-2.0. This is a local research build under the existing Foundation distribution restriction.
