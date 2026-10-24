# Android checkpoint: 2026-10-24

This checkpoint produces two installable **platform validation APKs**. Neither
launches a game. Game modules and shader data remain private, outside Git and
outside the APKs. No Zelda gameplay, rendering correctness or FPS is claimed.

## Source and dependencies

The GitHub fork is `tencentmalos/MacNativeGames`, parent
`ly05010419/MacNativeGames`. Development branch `codex/android-zelda` was advanced
from the fork's older main to upstream `5281186` before these changes. The pinned
Citron reference checkout was not modified.

Foundation and Oboe are consumed from the exact clean source revisions in
`dependencies.json`. Foundation supplies the Android controller collector,
InputHub, callback mixer and Oboe output. No Foundation source changes were
required. See `THIRD_PARTY_NOTICES.md` for the combined Android application's
licenses.

## Verified preparation

| Title | Exact input | Android rewrite emission |
| --- | --- | --- |
| BOTW | Switch 1.6.0, main Build ID `8E9978D50BDD20B4C8395A106C27FFDE` | main, sdk, subsdk0: 3/3 |
| TOTK | Switch 1.0.0, main Build ID `082CE09B06E33A123CB1E2770F5F9147709033DB` | main, sdk, subsdk0: 3/3 |

All six full input hashes were checked against the existing version manifests.
The Android path uses Bionic TLS and allocated stack scratch space, retains the
Zelda-specific jump-table/exclusive-window handling, and supports the original
BLR x18 call windows without changing the return PC. Unsupported SP rewrites
fail. There is no below-SP fallback on Android.

Eight call-window differential tests passed, including recursion, preserved
registers/return PC and simulated signal stack clobber. The general Android
rewriter test passed 192 synthetic instruction/seed combinations, each compared
both normally and with below-SP clobber at instruction boundaries. These tests
contain no game code; they do not prove equivalence of every rewritten game
instruction.

The BOTW and TOTK main modules were also rewritten using both unmodified
upstream and the fork's default Apple path. The resulting images and all module
tables were byte-identical. Only `build_src.sha256`, which identifies the
modified generator, differed. This is a preparation regression check, not a
Mac gameplay run.

## Build and device checks

- Upstream Python suite: 331 tests passed with Xcode 26.
- Host CMake/CTest: 3/3 passed (adapter, Foundation audio and Foundation input).
- The real Zelda `audio_mixer.c` compiled for Android, macOS and iOS. Full Apple
  game bundles were not rebuilt. Android currently compiles this mixer as an
  object; the full guest runtime is not linked into the platform APK.
- Both APK flavors built with JDK 17, Gradle 9.8.1, AGP 9.4.1, SDK 36 and
  NDK 29.0.14206865. Both lint runs completed with 13 warnings and zero errors;
  warnings include newer tool versions, ARM64-only support and test UI polish.
- AYN Odin 3, Android 15: both instrumentation suites passed 2/2 tests. The
  250 ms tone submitted 10,080 frames; device callbacks consumed 10,032 frames
  for BOTW and 9,936 for TOTK before the measurement ended. This validates PCM
  consumption, not listening quality or game audio timing.
- The native input adapter tests also passed on the device. JNI tests exercise
  synthetic button/stick events, focus release and stale-session rejection.
  Both activities started and the collector detected the built-in controller.
  Physical button coverage, rumble and gyro have not been tested.
- Both APK signatures and ZIP 16 KiB alignment checks passed. Each APK contains
  ARM64 native libraries and no `assets/` entries or game input files.

## Local artifacts

The debug-signed packages are `org.macnative.android.botw` and
`org.macnative.android.totk`, version `0.1-platform`, minimum Android 12.
They are separate from the existing Citron and D2R packages.

| APK | Bytes | SHA-256 |
| --- | ---: | --- |
| BOTW-Android-platform-arm64-debug.apk | 5,407,359 | `e39a465d368b1a0af3fa7cf2b5b661d6dbc87f4a251b97e15b2afd32249ae7d6` |
| TOTK-Android-platform-arm64-debug.apk | 5,407,359 | `662f3755ab6c74b99895c09c3da3d0ea1e60e499640e293aa890bbf33f885cf8` |

## Next executable milestone

1. Port `loader.c`, `gate.S`, `hle_stack.S`, `thread_enter.S` and thread services
   to Bionic/ELF. Measure and cross-check pthread-key value offsets per process;
   do not use Apple's `pthread_key_t * 8` key-page values. Gate game entry on the
   exact module identities and verified Android layout marker. Validate host
   stack restoration, platform x18 preservation and guest TLS on hardware.
2. Link the existing Zelda Horizon services and Foundation-backed mixer/input
   into that host. Replace actual Darwin services where required; do not add
   success-returning service stubs just to make the library link.
3. Connect the NVN surface to the D2R Vulkan backend and generate Zelda-specific
   XGS bundles from the existing shader inventory/reflection. The earlier 18
   SPIR-V sample checks do not prove descriptor compatibility or full coverage.
4. Add private content import, lifecycle and saves, reach a rendered game frame,
   then measure cold/warm pipeline compilation and same-scene gameplay. Offline
   SPIR-V still requires driver-specific Vulkan pipeline compilation.
