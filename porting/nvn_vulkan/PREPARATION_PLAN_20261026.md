# NVN preparation plans: first-stage verification

Date: 2026-10-26

## Scope and outcome

The shared Mac/Android NVN-to-Vulkan adapter now reuses stable preparation work according to relevant state changes. This is the first stage of the optimization proposed after the Odin3 PSO warmup analysis. It does not introduce asynchronous translation or parallel Draw publication.

Implementation is in MacNativeGames adapter sources. The pinned D2R checkout and Citron's separate NVN frontend were not changed. Existing unrelated working-tree modifications were preserved. Nothing was committed or pushed. The initial host-verification stage did not deploy; the subsequently authorized Thor runtime test is documented separately below.

Correctness gates pass on the host and its synthetic Vulkan GPU tests. Subsequent [Thor runtime A/B](THOR_PREPARATION_AB_20261026.md) and [Odin3 runtime A/B](ODIN3_PREPARATION_AB_20261027.md) both **did not demonstrate an FPS benefit**. Gameplay screenshots provide bounded comparative visual coverage, not full rendering conformance. Thor used a separate validation-save copy; Odin3 reused existing data, with one runtime-written `option.sav` difference explicitly recorded and backed up in its report.

## Implemented changes

### Structural state revisions

`prepare_preparation.py` adds revision updates at decoded state application points. Equal values do not invalidate structural preparation. Partial stream/descriptor binding updates, complete vertex-attribute replacement, initialization, shutdown, and temporary clear views are covered.

Vertex/blend plans check revisions instead of comparing their whole arrays on every Draw. The compact early pipeline guard also checks actual selected shader identities, layout, render-pass identity/compatibility inputs, topology, samples, active colors and depth facts. The original full pipeline key, ownership, invalidation and LRU touch remain authoritative.

The existing `buf_gen`, `tex_gen`, resource revisions, snapshot epochs and slot serials remain intact. Buffer binding shape/address revisions are separate: a changed nonzero dynamic-UBO address does not by itself change descriptor identity, but unbinding, length changes and static buffer addresses do.

### Shared binding and read plans

`preparation_plan.h` and `preparation_plan_engine.inc` compile immutable binding metadata for the actual selected Shader. The metadata feeds dynamic-mode counts, descriptor preparation and read preflight. Storage alias range shapes are merged once per relevant binding change and shared by preflight and descriptor construction.

Only the shapes are cached. Every operation still checks current pending GPU dependencies. Vertex/index reads, newest-overlap rules, resource lookups, current UBO snapshots and image preparation remain live operations.

Plans copy metadata by value and do not retain pointers into resizable Shader binding vectors. Border fallback clears copied plans after extending the Shader metadata, including previously derived uniform counts. Plans are discarded before shader destruction. Limits are 4096 elements per Shader/active combined plan and a 16 MiB shader-metadata budget; excess inputs use the conservative renderer path.

### Descriptor identity reuse

`preparation_key_engine.inc` caches the exact binding-dependent key portion. The current feedback identity, selected shaders and pool facts are still included at lookup time. Descriptor memo pinning, epochs, slot ownership, registration validation and resource lifetime checks remain unchanged.

A key hit still refreshes current uniform data/dynamic offsets and driver/support blocks, checks texel/storage data and prepares images. It never treats an unchanged address as evidence of unchanged contents. Existing `DescMemo` canonical dynamic-UBO ordering is reused, rather than introducing another ordering convention.

Shader variant selection itself is unchanged; plans describe its actual selected result.

### Controls and observability

The shared policy defaults to:

- `MNG_PREPARATION_PLAN=1`
- `MNG_PREPARATION_VALIDATE=0`

Debug commands:

```text
renderer_schedule plans on
renderer_schedule plans off
renderer_schedule status
```

Android properties transport the same options as `debug.mng.preparation_plan` and `debug.mng.preparation_validate`; there is no separate Android default. Toggling uses the existing nonblocking engine-lock contract and invalidates preparation fronts without draining the GPU.

Validation mode recomputes legacy descriptor keys and full pipeline identities, and fails on disagreement. It is for correctness testing, not performance measurement. Capture/provenance/UI exclusions and unsupported metadata retain conservative fallback paths.

Status includes plan/key/range builds and reuse, pipeline reuse, metadata bytes, metadata/legacy-key fallback counts, and memo-ineligible keys. Submission-level counters avoid per-binding timestamps or unbounded event logging.

## Verification results

### Builds and host tests

- Mac shared Vulkan adapter, `mng_vulkan_probe`, preparation/policy/schedule probes and relevant existing host targets: built successfully.
- Mac `vulkan_runtime.cpp`: compiled against the current callback headers using the isolated Mac target flags.
- Android `mng_vulkan_probe`, `mng_preparation_plan_probe`, `mng_profiler_tests` and `mng_schedule_probe`: built successfully. These Android executables were not run on a device.
- Seven relevant porting CTests plus the shared schedule CTest: **8/8 passed**.
- Existing porting Python tests: **23/23 passed**.
- Foundation `mng_keyed_jobs_probe`: passed concurrency, deduplication, claim/wait, expected failure, budget and shutdown checks. It prints existing duplicate reflection-registration diagnostics while reinitializing workers; the expected failure test reports `failed=1`, not an unhandled test failure.
- Preparation tests compile the production plan/key-cache code against synthetic renderer state. They check revisions, partial changes, array ordering, metadata ownership, storage merging/reuse, border-style cloning, budget fallback and texel-mode changes, plus **180000 randomized legacy-key comparisons**.
- The preparation tests also passed **AddressSanitizer and UndefinedBehaviorSanitizer** with the explicit Xcode 26 toolchain.
- Source whitespace checks and `git diff --check`: passed. The pinned D2R working tree remained clean.

### Actual GPU matrix

Host: Apple M4 Pro. The existing independent diagnostic MoltenVK 1.4.2 library was used; no Citron/system driver was replaced.

Driver SHA-256:

```text
8ea25172b62b40919bcb60d24c7d93970ac625921a38eb625e66d57f52e8ea8e
```

Final probe binary SHA-256:

```text
9225e8e353bfb5784a14c2d8aef377c704e4bf5e6eb38b7ecbabc27162248639
```

All five cases enabled `MTL_DEBUG_LAYER=1`, `MNG_PREPARATION_VALIDATE=1`, `MNG_PIPELINE_CACHE_ENTRIES=4`, and asynchronous MoltenVK queue submission. No validation errors were found in the final logs.

| Case | Result | GPU submissions | Draws / dispatches |
|---|---|---:|---:|
| Preparation explicitly off | PASS | 396 | 174 / 10 |
| Preparation explicitly on | PASS | 396 | 174 / 10 |
| Preparation on, border fallback explicitly forced | PASS | 396 | 174 / 10 |
| Preparation on, legacy dynamic-UBO plan | PASS | 396 | 174 / 10 |
| No preparation environment override: shared default | PASS | 396 | 174 / 10 |

The MoltenVK configuration already needs the border fallback, so the forced-border case is not evidence of switching between native and emulated border implementations.

Each run retained the existing full pixel/word checks, including 8208 BC block words, 6144 custom-border pixels, aliasing, GPU writeback chains, feedback, slot reuse and hot-pipeline LRU. The added regression verifies **8192 pixels** across repeated identical state bindings and same-address UBO content changes, including bound-length changes and hot preparation toggles. Existing dynamic-UBO array/source tests verify another 16384 pixels.

The on/default cases reported:

| Preparation operation | Builds | Reuses |
|---|---:|---:|
| Binding-dependent descriptor key | 22 | 148 |
| Storage range shapes | 20 | 21 |
| Compact pipeline guard | — | 107 |

These counts prove that the new paths execute and retain the expected outputs. They are **not a gameplay speedup or FPS measurement**. Depth-bounds pixels remain zero because this MoltenVK device configuration does not support that feature.

### APK

`assembleBotwDebug` and `lintBotwDebug` completed successfully using existing pinned dependencies and the same hash-verified Turnip input. The final APK was inspected as a ZIP: its `libmacnative_platform.so` contains the new preparation counter, option and metadata-status markers.

- APK: `android/app/build/outputs/apk/botw/debug/app-botw-debug.apk`
- Bytes: 16987781
- APK SHA-256: `8656c2deb000cc88ee8f6c931b3421e14894ae5981158dbf0cf83831ef425d26`
- Packaged `libmacnative_platform.so` SHA-256: `0012331936535fe9fb3b6711f940dd37171a317c2d39d9373b8969613cc9e65b`
- Packaged Turnip SHA-256, unchanged: `e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`

The APK was initially built without deployment. After the user separately authorized device testing, these same bytes were installed on Thor for the [runtime A/B](THOR_PREPARATION_AB_20261026.md).

## Unsuccessful attempts retained

- The initial probe used Homebrew MoltenVK 1.4.1 (`6979d03daad0474786d7601c835db2d6830dc50b47df9fdbce61acbabae60487`). Even with preparation off it failed the existing BC raw-view image case with `VK_ERROR_FORMAT_NOT_SUPPORTED`. The documented patched diagnostic library above passed the complete case; no test was deleted or silently skipped.
- An initial Gradle invocation omitted this checkout's required external dependency properties and failed during configuration. Passing the existing pinned paths produced the successful full APK/lint build.
- An initial unbounded sanitizer attempt through the default toolchain did not finish and was stopped. The subsequent explicitly selected Xcode 26 check had bounded compile/run timeouts and passed both sanitizers.

## Evidence and next validation gate

Local evidence is under `android/build/nvn-preparation/`:

- [Final GPU matrix](../../android/build/nvn-preparation/gpu-final-results.json)
- [APK package verification](../../android/build/nvn-preparation/apk-verification.json)
- [Task-only implementation patch](../../android/build/nvn-preparation/implementation.patch), relative to the session-start file contents rather than the repository's already-modified HEAD

The frontend still synchronously translates under the existing resource lock. Current UBO/image/content checks are intentionally not removed. This change therefore does not claim to eliminate the 26 ms Translate interval or explain the separate GuestCondition wait.

The first runtime follow-up completed on AYN Thor after Odin3 disconnected: [Thor preparation-plan A/B](THOR_PREPARATION_AB_20261026.md). OFF/ON/OFF/ON in one process produced pooled successful-native-present rates of 20.812 / 20.445 FPS, an observed -1.765% difference. The fast path executed, but this experiment did not demonstrate a performance gain. It compares runtime modes in the same new APK, not the total patch against an older binary.

The subsequently authorized [Odin3 test on 2026-10-27](ODIN3_PREPARATION_AB_20261027.md) completed in one process with OFF/ON/OFF/ON rates of 22.246 / 21.667 / 22.019 / 21.935 FPS. Duration-weighted OFF/ON was 22.132 / 21.801 FPS (-1.498%). This separate device result also did not demonstrate a gain; it is not pooled with Thor. No Root, frequency lock, uninstall or data clear was used. Installation preserved all 78 audited hashes; after gameplay, one backed-up `option.sav` had six changed bytes, explicitly retained as a preservation exception rather than silently overwritten. Full gameplay correctness and a precise causal regression magnitude remain unverified.
