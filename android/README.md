# BOTW / TOTK Android port

Development branch: `codex/android-zelda` in
https://github.com/tencentmalos/MacNativeGames, based on upstream `5281186`.
The fork already existed; its parent relationship was verified before editing.
The read-only Citron reference checkout is unchanged.

Measured results and remaining game-runtime work: [VALIDATION.md](VALIDATION.md).

## Current boundary

The two APK flavors are **platform validation applications**, not playable game
ports. They exercise the actual Foundation controller collector, InputHub, JNI
bridge and Oboe output. The UI states that game startup and Vulkan rendering are
not connected; there is no fake game launch path. No game modules, keys, saves,
RomFS files or translated shaders are packaged.

Implemented:

- Android/Bionic TLS and no-red-zone instruction rewriting, preserving the
  existing Zelda jump-table discovery and linked exclusive windows. Unsafe SP
  cases fail instead of falling back to below-SP scratch stores.
- Android-safe BLR x18 windows preserve the original return PC. The existing
  Apple default remains; Android images carry TARGET_ANDROID in their layout
  and the Apple table generator rejects them.
- Foundation Android controller collection and native InputHub, mapped by physical
  button position to Switch FullKey. Native session tokens and connection epochs
  reject stale input. Focus/pause ends the input session and releases buttons.
- The Zelda mixer has a Foundation audio output branch. A producer worker passes
  owned PCM to Foundation's callback mixer; Oboe's realtime callback never calls
  the guest mixer. Shutdown cancels pending submission and joins the worker
  before the mixer context is freed. The real mixer is compiled for Android as
  part of the APK build, but the full guest runtime is not linked into this APK.

Not implemented: Android module loader/gates and Horizon runtime integration,
NVN-to-Vulkan connection and Zelda XGS generation, game lifecycle/saves/import,
rumble dispatch, gyro and on-screen controls. Haptics are not advertised as a
capability by the current native adapter. No Android Zelda FPS result exists.

## Build

Use JDK 17, SDK 36, NDK 29.0.14206865 and SDK CMake 3.22.1. Obtain the source
checkouts in `dependencies.json` at the exact commits (Foundation requires access
to the owner's repository). CMake rejects a mismatched checkout. Source modules
are consumed directly; Foundation is not copied or modified.

```sh
cd android
./gradlew :app:assembleDebug :app:lintBotwDebug :app:lintTotkDebug \
  -PfoundationDir=/absolute/path/to/foundation \
  -PoboeDir=/absolute/path/to/oboe
```

Packages: `org.macnative.android.botw` and `org.macnative.android.totk`.
Both require Android 12 or newer and ARM64. The wrapper pins Gradle 9.8.1 with
its distribution SHA-256. Foundation Kotlin sources use the AGP 9
[AndroidSourceSet.kotlin API](https://developer.android.com/build/migrate-to-built-in-kotlin).
See `THIRD_PARTY_NOTICES.md` for the Android dependency licenses.

```sh
./gradlew :app:connectedBotwDebugAndroidTest \
  -PfoundationDir=/absolute/path/to/foundation \
  -PoboeDir=/absolute/path/to/oboe
```

The instrumentation tests submit synthetic controller events and play a quiet
250 ms tone, requiring the Foundation device callback to consume PCM. They do
not certify physical controller mappings or audible output quality.

## Prepare game modules privately

Run `games/totk/android/prepare_modules.py --help` with Python 3.11+ and pass a
Python interpreter containing Capstone and LZ4. Supply original BOTW 1.6.0 and
TOTK 1.0.0 ExeFS directories, never already-rewritten images. It verifies all six
NSOs against the existing expected-version manifests before writing anything.
Outputs must be a new directory outside Git. Each result records module Build
ID, complete input hash, rewriter/call-window hashes, output hashes and exit
status. A failed module makes the command fail after retaining the report.
Emission alone is not equivalence, boot or gameplay validation.

Synthetic differential tests (Unicorn is test-only):

```sh
python games/totk/scripts/test_call_window.py
python games/totk/scripts/test_android_rewrite.py
python -m unittest discover -s tests
```

Use Xcode 26 for the upstream Apple tests. The old Xcode 16 SDK cannot compile
one of the upstream MetalFX tests. Full Mac/iOS game bundles have not been
rebuilt in this Android step; the shared mixer was compiled for both SDKs.
