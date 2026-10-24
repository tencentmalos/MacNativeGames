# Android runtime checkpoint: 2026-10-24

The BOTW and TOTK APKs now import prepared modules and execute original ARM64 SDK
functions inside the Android application. **Neither starts a game or renders a
game frame.** No gameplay, visual correctness or frame-rate result is claimed.
Game programs, RomFS, shader data, keys and saves remain outside Git and the APKs.

## Source and dependencies

Fork: `tencentmalos/MacNativeGames`, parent `ly05010419/MacNativeGames`.
Branch: `codex/android-zelda`, based on upstream `5281186`. The read-only Citron
reference checkout is unchanged. Foundation and Oboe use the exact clean source
revisions in `dependencies.json`; Foundation supplies input and audio. No
Foundation changes were required. Android includes GPL components as documented
in `../THIRD_PARTY_NOTICES.md`.

## Executable identity and preparation

| Title | Version | Main Build ID |
| --- | --- | --- |
| BOTW | Switch 1.6.0 | `8E9978D50BDD20B4C8395A106C27FFDE` |
| TOTK | Switch 1.0.0 | `082CE09B06E33A123CB1E2770F5F9147709033DB` |

All six original NSO hashes match the existing version manifests. The Android
rewriter preserves the Zelda jump-table/exclusive-window handling, uses Bionic
TLS and allocated stack scratch, and rejects unsupported SP windows. The
previous preparation checks passed eight call-window differential tests and 192
instruction/seed cases with simulated below-SP clobber. Those synthetic tests
are not proof of every rewritten instruction or of gameplay.

`package_modules.py` independently reads dynamic symbols and RELA/RELR tables
from each rewritten module. The original clock import identifies its PLT; no
address from another game or SDK version is transplanted. Packaging was repeated
from the verified report and reproduced both pinned manifest hashes:

| Profile | Manifest SHA-256 |
| --- | --- |
| botw160 | `30513eed53bc4ae9bd1539c9e28dbffdb859b9c2c054fefd05aa9e0de3054f38` |
| totk100 | `8878725de0e5776b4dbec4654dbaec1c5cb5484f395a24796e6ab7f2ffe898d0` |

The APK pins the complete manifest before parsing its tables, then verifies
module image hashes. This prevents a changed image being accepted merely by
editing its hash declaration. Updating the rewrite generator requires reviewing,
regenerating and validating the profile pins together.

## Application runtime validation

Device: AYN Odin 3, Android 15, 4 KiB host pages. Both debug APKs were updated in
place, preserving their data and other applications. Each flavor passed all
three instrumentation tests, including the private-module test (not skipped).

For **each** title, inside the APK:

- Import, relocate and protect all three modules. Resolve strong imports, report
  weak zero bindings, and verify every page's actual protection in `/proc/self/maps`.
- Keep guest modules, stacks and TLS below `2^39`, without replacing existing
  mappings. Code is RX, mutable data RW, and host/guest stacks have guard pages.
- Discover Bionic pthread-key offsets from two markers and cross-check the slots
  on each worker. Preserve Android's platform x18 and restore the host-stack slot.
- Execute 4,000 calls through original main PLT -> SDK -> timer gate on four
  native threads; require monotonic results and exact gate counts.
- Execute original SDK `SleepThread` once per worker for 15 ms, require one
  positive-duration SVC wait and no early return. Interrupted host waits retry.
  Scheduler-yield variants and other SVCs are unsupported, not successful stubs.
- Reject the other title profile, a corrupted manifest, a corrupted main image,
  a truncated SDK image and missing files. Failed imports retain the selected
  working generation and remove only their own incomplete copies.
- Run a second full native session to exercise key/mapping teardown and reentry.
- Pass Foundation synthetic input session/focus/stale-packet checks and require
  PCM consumption from its real device audio callback. These do not establish
  physical button coverage or listening quality.

The UI exposes folder import, native-call verification and report sharing. Import
uses the system document-tree API and an app-private staging directory; selection
is published atomically only after native validation. A worker is retained at
application scope across Activity recreation and does not share the input queue.
The transactional import and execution were tested inside the app. A full manual
system-picker grant/selection cycle and rotation-during-import have **not** yet
been completed: the device foreground switched to another application during
picker validation, and subsequent checks ran in the background.

This does not execute `nn::init::Start`, DT_INIT constructors, `nnMain`, NVN or
Vulkan. No game state or save was opened or modified.

## Build checks and local artifacts

- Upstream Python suite with Xcode 26: **332 tests passed**.
- Both APKs and both instrumentation APKs built with JDK 17, Gradle 9.8.1,
  AGP 9.4.1, SDK 36 and NDK 29.0.14206865.
- Both lint runs passed with zero errors. Remaining warnings include newer
  toolchain versions, ARM64-only targeting and development-screen text/icon polish.
- APK signing and ZIP 16 KiB alignment checks passed. The APKs contain the ARM64
  native library and no `assets/` entries. Actual execution on a 16 KiB Android
  device remains untested; the Odin 3 uses 4 KiB pages.
- The shared Apple runtime sources were not modified in this step. The preceding
  checkpoint compiled the shared mixer for Android, macOS and iOS and compared
  Apple BOTW/TOTK rewrite outputs against upstream. Full Mac/iOS game bundles
  were not rebuilt in this Android-only loader/application step.

Package IDs remain `org.macnative.android.botw` and `org.macnative.android.totk`,
version code 2, version name `0.2-runtime`, ARM64, minimum Android 12.

| APK | Bytes | SHA-256 |
| --- | ---: | --- |
| BOTW-Android-runtime-arm64-debug.apk | 7,136,503 | `e1e847083f1939668e0a84d96fc8b45b1238395d43ce0a9ab13d1e31a1322620` |
| TOTK-Android-runtime-arm64-debug.apk | 7,136,503 | `3b9e3c72de9c1f43945c9c98434a4b017cd352c652c14557a7adc07317eefade` |

## Remaining work before gameplay

1. Connect original SDK initialization and constructor order to complete Android
   Horizon thread, memory, synchronization and IPC services. Port the existing
   Darwin host dependencies without hiding missing services behind success stubs.
2. Connect the NVN surface to Vulkan and prepare Zelda-specific shader bundles
   with matching reflection/descriptor layouts. D2R's game shader bundles cannot
   substitute for Zelda shaders. Offline SPIR-V still needs driver-specific
   Vulkan pipeline compilation.
3. Connect RomFS, private saves, app lifecycle and the Foundation-backed game
   mixer/input to that runtime. Reach an actual rendered game frame, then verify
   correctness and measure cold/warm compilation and same-scene performance.
