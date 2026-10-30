# Thor preparation-plan runtime A/B

Date: 2026-10-26

## Outcome

The requested same-scene OFF/ON/OFF/ON test completed on **AYN Thor**, not Odin3. Odin3 disconnected before deployment; the user explicitly selected Thor as the replacement.

**No performance improvement was demonstrated.** The new preparation reuse path was approximately 1.76% slower in pooled successful-native-present cadence in this experiment. Both ON runs were below the adjacent OFF runs. These are measurements from an unlocked, charging device, not a statistically established hardware-independent regression magnitude.

This is a same-APK runtime-toggle comparison. The OFF path still contains shared, unconditional revision bookkeeping introduced by the preparation implementation. It is **not** a comparison with the pre-change APK and does not isolate the total cost of the patch.

## Identity and controls

- Device: AYN Thor, serial `9c2841a4`, QCS8550 / Adreno 740v2.
- Android build: `qti/kalama/kalama:13/TKQ1.231222.001/eng.Thor.20260206.163241:user/release-keys`.
- Package: `org.macnative.android.botw`; all four cadence windows used native process **5148**.
- BOTW Switch 1.6.0, main Build ID `8E9978D50BDD20B4C8395A106C27FFDE`.
- APK SHA-256: `8656c2deb000cc88ee8f6c931b3421e14894ae5981158dbf0cf83831ef425d26`; verified after installation.
- Packaged/selected Turnip: Mesa `68dafb89a2`, SHA-256 `e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`.
- 720p handheld, internal scale 1x, existing 60 FPS Guest patch.
- Only `renderer_schedule plans on/off` changed between cases. Descriptor plans, image fast path, early pipeline path, read preflight, batching, presentation and all other checked controls remained unchanged.
- `preparation_validate=0`, `perf_sample=0`, detailed CPU tracing off. Profiler recording was off for every cadence window. Separate three-second files were collected after those windows.
- Runtime PSO worker count was 4; the launch override remained automatic (`pso_workers=0`). No PSO/shader creation appeared in the sampled steady-state cadence rows.

The capture `app_info` text records launch-time options. It does not update when `plans` is toggled; per-case `renderer_schedule` snapshots and preparation-counter deltas are authoritative for the actual mode.

## Scene establishment

Thor did not previously have this test package. The already prepared private modules, RomFS/DLC, shader bundle, fonts and a separate validation-save copy were installed into new test locations. Existing applications and user data were not cleared or overwritten. Source game assets and the original save were not modified.

The imported module manifest SHA-256 was `30513eed53bc4ae9bd1539c9e28dbffdb859b9c2c054fefd05aa9e0de3054f38`; the application's existing importer reported `status=pass`, `phase=validated`. RomFS content SHA-256 matched `89d922b7b01f09766530a601f3dcfca8bdd4e94e6387f1c1335c4f3c6662b625`.

Initial launches correctly refused resources whose `ctime` changed during first access despite matching full content hashes. Those failures were retained. Later retries passed without disabling or weakening the integrity checks.

The first-run DLC page prevented the A-only autoloader from finishing; its timeout was not relabelled as successful automation. The test then returned with B and explicitly selected Continue, the sole imported Shrine of Resurrection save dated 2026-10-22 16:49, and Start. Menu/confirmation screenshots were retained. A fresh, sustained gameplay workload with at least 1000 draws, dispatch work, advancing presents and no new shaders/pipelines was required for 20 seconds before measurement.

All four windows remained in the same process at the same standing camera/character position, with no injected input between measurements. Screenshots show matching scene composition and no obvious new OFF/ON visual difference. This is comparative visual coverage, not complete rendering conformance or an instrumented proof of exact Guest coordinates.

## Unprofiled cadence

The nominal window was 45 seconds; actual sample-age-corrected intervals were approximately 46.5–46.7 seconds. The metric is **successful native Vulkan presents**, not compositor display timestamps or independently verified unique Guest frames.

| Case | Plans | Successful presents | Measured interval | FPS |
|---|---:|---:|---:|---:|
| OFF A | 0 | 973 | 46.684 s | 20.842 |
| ON A | 1 | 949 | 46.505 s | 20.406 |
| OFF B | 0 | 968 | 46.578 s | 20.783 |
| ON B | 1 | 954 | 46.574 s | 20.484 |

- Duration-weighted OFF: **20.812 FPS**.
- Duration-weighted ON: **20.445 FPS**.
- Observed pooled difference: **-1.765%**.
- Adjacent OFF-to-ON differences: **-2.091%** and **-1.438%**.
- OFF repeat drift: **-0.286%**; ON repeat drift: **+0.378%**.
- Sampled median draw counts: 3720.5, 3655, 3690, 3720; median dispatch count was 14 in each case. The scene is animated, so per-frame work is not byte-identical.

No CPU/GPU frequency lock was applied. Android's thermal service reported `HAL Ready: false`, so its status-zero replies are **not evidence that temperatures or throttling were unchanged**. Battery temperature was 40.0 C in the post-run sample. These limits prevent treating the small difference as an exact causal speed ratio; they do not provide evidence of a gain.

## The fast path really executed

The ON-window preparation counter deltas were:

| Counter | ON A | ON B |
|---|---:|---:|
| Binding builds | 345380 | 358073 |
| Binding reuses | 15487496 | 16032360 |
| Key builds | 987074 | 1021596 |
| Key reuses | 2599203 | 2688931 |
| Storage-range builds | 351501 | 364193 |
| Storage-range reuses | 3318368 | 3434073 |
| Compact pipeline reuses | 2839682 | 2938054 |
| Metadata/legacy-key fallbacks | 0 | 0 |

The corresponding OFF-window deltas were zero. More reuse counters therefore did not translate into improved cadence. In particular, counters alone must not be presented as an optimization success.

A concrete implementation concern for later attribution is the single active graphics BindingSet: changing shader combinations rebuilds its flattened groups and the revision watches for all three dynamic-UBO modes. This creates substantial bookkeeping even with many later hits. This is a source-backed candidate, **not a measured attribution of all the observed difference**; no further cache tuning was made during this validation.

## Profiling integrity and CPU validation

All four initial three-second containers finalized and transferred with matching SHA-256. The first file had complete CPU payload coverage with capture-boundary unclosed endpoints. The following files contained records from an earlier time range and unbalanced endpoints after recording-mode transitions; their index diagnostics marked CPU integrity truncated. Those files are preserved, but are not promoted to lossless causal CPU comparisons merely because their containers have footers.

A separate fresh process (**13906**) performed its **first** CPU file capture continuously while toggling OFF/ON/OFF/ON. Each segment recorded the runtime SDK frame counter before/after the toggle and excluded two source frames at both boundaries. Native-present tokens provide the explicit application/source-frame bridge. This capture is for CPU validation, not a replacement FPS benchmark.

Continuous capture:

- Raw bytes: 146440318.
- SHA-256: `ce3332c250c3d2904db3e95ffb2ff1671745d9faf0d7335ae95c6ea79c2ef455`.
- Safe SDK frame ranges: OFF A 22–77; ON A 105–160; OFF B 202–256; ON B 288–341.
- Index: `d9ac64fb8a974103a2053fad5c66fe7f`; 19286992 events, 353 source markers / 352 complete source frames, 1725 chunks accepted, zero chunks skipped, zero dropped-chunk markers, container complete, CPU `truncated=false`.
- Translate owner: `BOTW-native`, TID **13982**. Header `main_thread_id=14032` identifies the presenter, not the translator.
- Capture-boundary unclosed CPU spans and an incomplete GPU zone/orphan GPU times remain explicit diagnostics. No calibrated CPU/GPU alignment or full GPU coverage is asserted.

The following elapsed scopes are clipped to complete marker-to-marker source frames. Each row contains only the safe interior of its runtime mode. Parent and child inclusive times **must not be added**: FrontLock.Hold includes Translate, and Translate includes Draw/preflight work.

| Segment | Complete source frames | Draws/frame | Source-frame ms | Translate ms | Draw ms | Preflight ms | FrontLock.Hold ms | GuestCondition ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| OFF A | 56 | 3645.14 | 55.346 | 33.131 | 22.194 | 4.973 | 33.304 | 21.412 |
| ON A | 56 | 3642.70 | 54.825 | 32.352 | 21.091 | 5.505 | 32.454 | 21.432 |
| OFF B | 55 | 3649.80 | 56.147 | 32.822 | 21.755 | 5.080 | 33.242 | 21.871 |
| ON B | 54 | 3650.39 | 56.284 | 32.507 | 21.069 | 5.581 | 32.648 | 22.842 |

A separate complete-call pairing requires exactly one elapsed/thread-CPU/CPU-valid counter triplet inside each retained Translate span; all 221 selected calls passed that check:

| Segment | Complete paired calls | Translate elapsed ms/call | Translate thread CPU ms/call | Elapsed minus thread CPU ms/call |
|---|---:|---:|---:|---:|
| OFF A | 56 | 33.104 | 32.661 | 0.443 |
| ON A | 56 | 32.324 | 32.057 | 0.266 |
| OFF B | 55 | 32.795 | 32.478 | 0.317 |
| ON B | 54 | 32.478 | 32.148 | 0.330 |

Frame-weighted OFF/ON Translate thread CPU was **32.570 / 32.102 ms**, an observed -1.44%; clipped Translate elapsed was **32.978 / 32.428 ms**, -1.67%. Draw decreased from 21.977 to 21.080 ms (-4.08%), but preflight increased from **5.026 to 5.542 ms (+10.27%)**, consistently in both OFF-to-ON pairs. Profiled mean source-frame duration changed only from 55.743 to 55.541 ms (-0.36%), while GuestCondition increased from 21.640 to 22.124 ms. These short, instrumented segments do not establish a net gameplay gain and do not overturn the independent unprofiled cadence result. They also do not attribute the unprofiled -1.765% difference to a particular scope.

Translate remains predominantly actual thread CPU work: the mean paired elapsed/CPU gap is only 0.27–0.44 ms, not tens of milliseconds of measured blocked time. Its long frontend hold remains 32–33 ms. Small FrontLock.Wait values on this owner lane are not measurements of collateral API-thread blocking, and the separate GuestCondition interval is not proven to be caused by that frontend mutex. Without a scheduler sidecar, elapsed-minus-thread-CPU is not a runnable/sleep decomposition.

Preparation key-reuse deltas were 0 / 145057 / 0 / 140270, independently confirming the runtime modes. The read-only reduction verified the raw SHA, exact SDK ranges and one native-present token per selected source frame. Litep `analyze_frame_budget` independently matched all reported OFF A and ON A scope means/counts. The complete distributions, p50/p90 values and SDK-to-native-token mapping are retained in `continuous/cpu-summary.json`.

## Cleanup and source changes

The cadence script restored the original runtime mode (`plans=1`). The fresh continuous-capture script also restored `plans=1` and profiler recording to off. The temporary autoload property was restored to its prior empty value. Final read-only checks passed: process 13906 remained alive, successful presents advanced from 40761 to 40788, sample age was 8.65 ms, `recording=0`, `capture_active=0`, `detail=0`, `preparation_plan=1`, and `preparation_validate=0`. The healthy game was left running. A schedule-status try-lock returned busy once; the bounded retry succeeded without changing execution mode. No root command, frequency lock, package uninstall or data-clear command was used. New test resource directories and raw evidence remain available for reproduction.

The only new code for this validation is the existing benchmark tool's `--plans-only` mode, mode/status checks, screenshots/thermal snapshots outside the FPS intervals, and three host tests covering mode ordering, mutual exclusion and restoration on failure. All 26 related Python tests passed; source syntax and whitespace checks passed. The renderer, pinned dependencies, game modules and tested APK were not modified during the A/B.

The long `front_mutex` implementation has not been changed. Its ownership/order design is separate and remains unapproved. Removing that lock will not automatically remove the CPU work inside Translate or prove the cause of the separate GuestCondition wait.

## Local evidence

Private raw evidence is under:

`/Users/bytedance/workspace/native-switch-study/android-translate-20261026/thor-preparation-20261026/`

Key entries: `deployment.json`, `module-validation.json`, `manual-scene.json`, `cadence-summary.json`, `ab/result.json`, per-case JSONL/screenshots/raw PROF plus producer/hash sidecars, initial failed scene reports, `continuous/result.json`, `continuous/cpu-summary.json`, `continuous/reduction.log`, `analyze_continuous.py`, and the final runtime-state/health checks. Raw game assets and saves remain private and are not part of the repository report.

| Initial trace | SHA-256 | CPU use |
|---|---|---|
| OFF A `file-1793021934635979-1.prof` | `0a26e1cbd28ad8b30de3d34e365b371c0945161ab1642190c9bce54e0a682e90` | First capture; bounded complete frames available |
| ON A `file-1793021992757985-2.prof` | `ad72cc30f2f6d3ab16a6479257ff131be5d9de5870d04c0a06a2b4fd8fa3dee4` | Retained; transition/pairing diagnostics |
| OFF B `file-1793022050753523-3.prof` | `650c7a635b9177d748cd0c1a65fb8fc8fd9cffce3761bcf4ca62d7131c4514f2` | Retained; transition/pairing diagnostics |
| ON B `file-1793022112061971-4.prof` | `1ee7913c05f453e8d34c13725c9d809f3bde6aee229e20bfc1caac6a7bf669fd` | Retained; transition/pairing diagnostics |

No Odin3 FPS conclusion, calibrated CPU/GPU critical path, full visual correctness claim or global-lock-removal result follows from these measurements.
