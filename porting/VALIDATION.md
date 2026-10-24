# BOTW port foundation validation

Date: 2026-10-24. Source baseline: `c7fcac6` on the owner's
`codex/android-zelda` branch, plus this checkpoint. Only BOTW 1.6.0 was advanced.
The existing TOTK APK was not rebuilt, installed or used for acceptance.

## Implemented and exercised

| Check | Result | Boundary |
| --- | --- | --- |
| Repository Python/native suite | 341 tests passed in 61.235 s | Includes a freshly compiled common core, catalog identity/dependency failures and resolver negative cases |
| BOTW catalog | 12 entries; both private host manifests matched the catalog's original-module pins | Inventory only; zero shared executable recipes enabled |
| Shared core on Odin3 | Passed | Wrong identity, original-byte mismatch, overlap, category opt-in, atomic failure, receipts and debugger ownership/generations |
| Mac BOTW executable | Built with warnings-as-errors; native probe passed | 4,000 original main PLT/SDK calls on four threads; constructors and game entry not run |
| Android BOTW native probe | Passed | 4,000 original calls and four SDK waits on four threads, separate guest stacks/TLS |
| Native metadata/resolver | Four actual loader snapshots passed on Mac/Android | Original SDK Start vendor symbol, exact gate-return mapping and explicit unknown for an interior rewrite address |
| Mac native debugger | Checkpoint breakpoint and stable all-stop descriptor read passed | Synthetic fixture; no guest stack reconstruction or full-game debugging claimed |
| BOTW APK build and lint | Passed | `assembleBotwDebug` and `lintBotwDebug` only |
| Installed APK tests | 3 passed, none skipped | Private module import/execution and failed-import preservation, synthetic controller events and audio callback consumption |
| APK signature/alignment | Passed | Debug signing and 16 KiB zip alignment; device execution was on 4 KiB pages |

Device: AYN Odin3, Android 15, ARM64, 4 KiB host pages. No root, application-data
clear or uninstall was used. Existing private input data and imported generations
were preserved. The instrumentation module test completed with status code 0;
it was not an assumption skip. The audio/input tests do not certify physical
controller layout or audible game audio.

The upstream suite emitted subprocess `ResourceWarning` messages. The reported
children were checked after completion; none remained alive. The suite returned
success. Foundation's JSON dependency also emits existing Clang literal-operator
deprecation warnings in Android builds; no dependency source was changed.

Toolchain: Xcode at `/Applications/Xcode.app`, JDK 17, Android SDK/build-tools 36,
NDK `29.0.14206865`, CMake `3.22.1`, Gradle `9.8.1`, AGP `9.4.1`. Foundation and
Oboe remain at the commits pinned by `android/dependencies.json`.

## APK identity

Installed package: `org.macnative.android.botw`, version code **3**, version
**0.3-foundation**. The debug APK contains no game modules, RomFS, keys, saves or
translated game shaders.

SHA-256:

```text
3a6eaa0dcc0744336eeb7851e362fecda1982748a82c3f5d59a5130f56c187bd
```

Local artifact:
`/Users/bytedance/workspace/native-switch-study/porting-20261024/artifacts/BOTW-Android-foundation-arm64-debug.apk`.

## Native debugger evidence

The existing native-debugger MCP was used with a bounded synthetic fixture and
the exported `mng_port_debug_checkpoint` function breakpoint. The successful
session was `d03b0d7e4a3b4576bdc7d319ebc7b01d`, stop epoch **2**, with all threads
stopped. ABI 1, descriptor size 40, even sequence 6, generation 1 and the 37-byte
payload were read as descriptor -> memory -> descriptor, with identical descriptor
values. The payload identified the fixture's checkpoint. No target helper was
called. The process resumed and exited with code 0, and the session was cleaned.

Fixture Mach-O UUID: `53A62419-29A1-3772-9227-CFE10CA49724`. The full debugger
target token, journal and read-memory evidence remain in the private capture.
These identify that fixture session, not the game executable.

The default Xcode 16.3 `lldb-dap` failed its initial attach. Selecting
`/Applications/Xcode.app/Contents/Developer/usr/bin/lldb-dap` worked. A second,
short fixture run exited before breakpoint installation; a bounded attach window
in the test fixture enabled the successful read above. All three owned sessions
were cleaned, including the failed attach. The production runtime has no such
wait. No debugger-owned game process was force-terminated.

The actual Mac BOTW loader separately published the common schema and passed its
native-call probe. The Android loader emitted `modules_sealed`, `sdk_startup`
and `startup_blocked` snapshots; each bound to its exact private manifest and
resolved the original four-argument SDK Start export. Android LLDB attach,
native-debugger provider integration, complete rewrite source maps, mixed
guest/host unwinding and thread wait graphs remain unimplemented/unverified.

## Current runtime boundary

The Android standalone startup probe still returns **blocked**, exit code 3,
at `GetInfo` information ID 12 (ASLR region address), after two successful
process-entropy queries. Constructors, `nnMain`, NVN Vulkan frames and gameplay
have not run. This APK is a runtime preview, not a playable BOTW port. There is
no Android gameplay FPS or shader-performance comparison from this checkpoint.

The Mac probe preserved existing patch behavior and checked the existing guards.
It observed 18,249 exclusive-load upgrades in text and 63 in rewritten code.
Those counts describe this exact Mac input only. Android's rewrite must be
enumerated and validated independently, including concurrency behavior.

The BOTW-owned catalog records compatibility, timing/input, graphics and
diagnostic adaptations. It intentionally does not claim that the legacy Mac
post-relocation code has been migrated into `apply_staged`. Static recipes,
base-dependent fixups and state callbacks need separate proof before enabling
them. Mac per-patch receipts remain explicitly unavailable in the debug snapshot.

No full iOS build was performed. The new Mac debugger adapter is excluded from
iOS. Full Mac game/application packaging and live gameplay were not acceptance
steps; the rebuilt target was the BOTW native loader executable.

Private evidence is retained outside Git at
`/Users/bytedance/workspace/native-switch-study/porting-20261024`, including
`upstream-tests-final.log`, `porting-android-test-final.txt`,
`botw-apk-tests-final.txt`, `botw-mac-snapshots-final.log`,
`botw-android-startup-snapshots-final.log`, `native-symbol-validation-final.json`,
`botw-apk-artifact.json` and the three debugger session exports. Game inputs and
generated manifests are not repository artifacts.
