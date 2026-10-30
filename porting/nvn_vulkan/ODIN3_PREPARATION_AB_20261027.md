# Odin3 preparation-plan runtime A/B

Date: 2026-10-27

## Outcome

The requested four-window same-scene test completed on **Odin3**. **No FPS improvement was demonstrated.** Duration-weighted OFF/ON successful-native-present rates were **22.132 / 21.801 FPS**, an observed **-1.498%** difference. Both ON windows were below their adjacent OFF windows, but the second pair differed by only -0.381%; unlocked clocks, animation and repeat drift limit causal precision. This is not a statistically established regression magnitude or a whole-patch comparison against the original APK.

Runtime controls were restored and the game remained healthy. The installation preserved all 78 inventoried file hashes. After gameplay, 77 remained identical and one `option.sav` had six changed bytes; that exception is explicitly retained below rather than claiming every save byte remained unchanged.

## Identity and authorization

The user explicitly authorized device-identity verification, installation of the previously delivered test APK, game restart and a same-scene OFF/ON/OFF/ON comparison on Odin3. They required preserving data/settings/caches, no Root and no frequency locking.

- Selected device: AYN Odin3, serial `6edb0593`, device/product `sun`, SoC `CQ8725S`, arm64-v8a, Android API 35.
- Build fingerprint: `qti/sun/sun:15/AQ3A.250728.001/eng.Odin3.20260616.191954:user/dev-keys`.
- Thor `9c2841a4` was also online. Every device command in this validation explicitly selected Odin3; Thor was not changed.
- Package: `org.macnative.android.botw`; warmed game process **23281**.
- BOTW Switch 1.6.0, main Build ID `8E9978D50BDD20B4C8395A106C27FFDE` (zero-padded in the module report).
- Test APK SHA-256: `8656c2deb000cc88ee8f6c931b3421e14894ae5981158dbf0cf83831ef425d26`.
- Original installed APK SHA-256: `b863cf67f4a7e3109a8a94f95aeb26f6a54334c9d9c0d399d95f82717bfd8a24`; copied locally before replacement.
- Selected Turnip SHA-256: `e61a373f8ac8b14dd31fbf21bdd618de2d493d40b8434e84b0b9236ca19d785d`, Mesa `68dafb89a2`.
- Existing resolution retained: 720p handheld, internal scale 1x, existing 60 FPS Guest patch.

## Data, settings and caches

No game process was running when Odin3 reconnected. The application was explicitly stopped before the preservation snapshot and in-place update. Installation used `adb install -r`, never uninstall or data clear. The original APK and a local tar backup of preferences, application cache, native cache, existing saves, account identity and overlay settings were retained privately.

All **78** inventoried save/settings/cache file SHA-256 values matched exactly before and immediately after installation. `shared_prefs/native-game.xml` also remained byte-identical. Its existing asset-tree URI, 720p selection and A/B button mapping were not replaced. The existing shared resources and shader files under `/sdcard/game/switch/native/botw160` were reused, not re-imported or overwritten. No new save was deployed.

The existing debug override `debug.mng.read_preflight=1` and other launch properties were recorded, not reset. `debug.mng.autoload` was temporarily set only by the existing scene-validation tool and restored in its finally path.

## Scene and measurement protocol

The original latest save loaded successfully through the existing autoloader. The scene gate required an opened save, fresh advancing successful native presents, at least 1000 draws, dispatch work and zero new shader/pipeline creation for **20 continuous seconds**. The verified standing scene is inside the Shrine of Resurrection; a pre-measurement screenshot is retained. No movement/camera input was injected after loading or between cases. Exact Guest coordinates were not instrumented, and the gate does not prove absence of physical user input.

All four cadence windows use process 23281 and the same warmed scene, driver, title, resolution and caches. Only `renderer_schedule plans off/on` changes. Each mode settles for three seconds and then measures a nominal 45 seconds. Sample timestamps are corrected by runtime sample age. Profiler recording, detailed CPU tracing, preparation validation and performance sampling are disabled during the cadence windows. Screenshots and thermal snapshots are outside those windows. A separate three-second PROF is collected after each window; its instrumentation is never counted as unprofiled FPS.

The metric is **successful native Vulkan presents**, not compositor display timestamps or independently proven unique Guest frames. The comparison toggles modes inside the same new APK. OFF still contains unconditional revision bookkeeping added by the preparation patch; this is not a measurement of the entire patch against the pre-change APK.

## Results

| Case | Plans | Successful presents | Sample-age-corrected interval | FPS |
|---|---:|---:|---:|---:|
| OFF A | 0 | 1018 | 45.761 s | 22.246 |
| ON A | 1 | 991 | 45.738 s | 21.667 |
| OFF B | 0 | 1007 | 45.734 s | 22.019 |
| ON B | 1 | 1003 | 45.727 s | 21.935 |

- Duration-weighted OFF: **22.132 FPS**; ON: **21.801 FPS**; difference **-1.498%**.
- Adjacent pair differences: **-2.604%**, **-0.381%**.
- OFF repeat drift: **-1.023%**; ON repeat drift: **+1.236%**.
- Sampled median draws: 3655.5 / 3647 / 3687 / 3645.5. Median dispatches: 12 / 14 / 14 / 14. The standing scene is animated and work is not byte-identical.
- No new shader loads or pipeline creations appeared in the cadence samples. ON A did build four preparation metadata records; that is not PSO compilation.
- ON A/B key-reuse deltas: **2699220 / 2710576**; binding-reuse deltas: **16036622 / 16106744**. Corresponding OFF deltas were zero. Metadata/legacy-key fallback counters were zero. The fast path executed without demonstrating a cadence benefit.
- All four screenshots were visually reviewed: character/camera composition matched, with no obvious new OFF/ON visual difference. This is not a full pixel-conformance result.

Thermal HAL was ready and returned overall status 0. Its cached and fresh sensor readings differed substantially (for example cached CPU values of 89.9 C versus fresh CPU values around 53–63 C at the last boundary), and an `ibat` status of 6 was also present. These heterogeneous readings must not be simplified into proof of constant temperature or absence of frequency effects. No frequency lock or thermal override was applied. Screenshot overlay FPS/CPU/GPU values are neither the benchmark metric nor process-exclusive utilization evidence.

### Capture provenance and limits

All four separate three-second files finalized, transferred with matching SHA-256 and exact PID/capture identity, and passed producer-side `capture_complete` checks. Container finalization is not proof of lossless CPU scope pairing, especially across repeated recording transitions. No comparative CPU-cost attribution is made from these files in this report.

| Capture | SHA-256 |
|---|---|
| OFF A `file-1793065574685934-1.prof` | `9ca11242e41d485f5f3231ceba61e34939e4a1c7f17b97a853b39d49b22b8531` |
| ON A `file-1793065630438957-2.prof` | `5e782504c2717b8142d45944cce763739a60ae3e814adbe2124e76cd9c42317c` |
| OFF B `file-1793065685582050-3.prof` | `0ef450ed9738962372d56b73f5bd38939007da310c40f94bb924c38157d39170` |
| ON B `file-1793065739627646-4.prof` | `3680f112bb866811f1e67be7242d748964fb1c603e31ccaa265543c0633e7c37` |

The first OFF A file was independently indexed: `1055b1e3d5c7461493fe5f6f02b74776`, 3370584 events, 70 markers / 69 complete source frames, 300 accepted chunks, zero skipped chunks or dropped-chunk markers, CPU `truncated=false`, container complete. Boundary-unclosed CPU spans and an incomplete GPU zone remain diagnostics. The other three CPU payloads were not independently indexed in this validation; no lossless cross-mode CPU comparison or calibrated CPU/GPU critical path is claimed.

The earlier [Thor experiment](THOR_PREPARATION_AB_20261026.md) likewise demonstrated no gain, but its FPS and CPU numbers are not pooled with Odin3. The two device-specific negative observations do not establish a precise universal slowdown.

## Restoration and preservation audit

Final read-only runtime check: PID **23281**, native presents **10313 → 10342**, plans restored to **1**, preparation validation **0**, ring recording **0**, capture active **0**, detailed CPU tracing **0**, temporary autoload property restored to its original empty value. Persistent Android preference values and application ID **10141** were unchanged. The healthy game was left running; no default renderer setting was changed.

All 78 inventoried files remain present, with no additions in the audited paths. **77 hashes remain identical; `option.sav` changed during game execution**, not installation. Its length remains 344 bytes; six bytes differ across four little-endian words at file offsets 56, 80, 192 and 280 (255→259 twice, 121→125 twice). The field meanings have not been established, so this is not labelled an unchanged-settings result or assumed to be only play time. No options menu was changed by the test. Original and post-run bytes are retained privately, with exact hash/difference evidence in `option-diff.json`.

The strict initial end-of-run byte-identity assertion failed on this file; the completed audit reports the exception instead of hiding it. No live save was overwritten or silently rolled back. All other audited save files, account identity, Android preferences and the audited caches remained identical. The original file is recoverable from `before-save-settings-cache.tar` if the user requests exact restoration after the game is stopped.

## Scope boundaries

No renderer code, APK bytes, pinned dependencies, game modules or game assets were modified as part of this validation. The existing benchmark tool and its previously passing tests are reused. No Root, frequency lock, data clear or uninstall is permitted or performed. Global frontend/engine locks remain unchanged; their separate read-only design is not an implemented optimization.

## Private local evidence

`/Users/bytedance/workspace/native-switch-study/android-translate-20261026/odin3-preparation-20261027/`

Identity/preservation evidence: `device-identity.json`, `before-inspection.json`, `before-installed.apk`, `before-save-settings-cache.tar`, `before-native-game.xml`, `preserved-files-before.json`, `preserved-files-after-install.json`, `deployment.json`, `launch-debug-properties.json`.

Scene/capture evidence: `scene/result.json`, `scene/events.jsonl`, `scene/guest-modules.txt`, `scene-ready.png`, `ab/result.json`, per-case JSONL/screenshots/PROF and exact producer/hash sidecars. Private saves, account data, raw game assets and backups are not part of the repository deliverable.
