# The offline tools on Windows and Linux

The game runs only on Apple silicon: its own ARM64 code executes directly and the graphics are Metal 4. The offline half
of the pipeline is ordinary .NET and Python, though, and this fork runs it on Windows (and Linux) to study the game's
shaders from another runtime: extracting and verifying the RomFS, translating every shader, and exporting what each one
needs at bind time. Nothing here changes the Mac or iPhone build.

Everything these steps write is derived from your own game files: keep it in a work folder, never in a repository.

## Requirements

| Tool | Notes |
|---|---|
| .NET 10 SDK | for building the translator |
| Visual Studio C++ build tools + Windows SDK | NativeAOT links with MSVC; `publish_native.py` puts `vswhere.exe` on PATH for it |
| Python 3.14 | `compression.zstd` (Tears of the Kingdom's RomFS). The NuGet `python` package works without installing anything system-wide; add `cryptography` with pip |
| SPIRV-Cross at `aa217aeb` (`build_app.py`) | built with CMake; a different SPIRV-Cross changes the MSL that the reflection is parsed from |
| hactool at `1d64a83` | Breath of the Wild only, under Linux or WSL |

Set `PYTHONUTF8=1` on Windows: the scripts read and write UTF-8 text, and the default code page is not UTF-8.

## Translator

```
python games/totk/tools/totk_xlate/publish_native.py --rid win-x64     # or linux-x64
```

It applies `patches/*.patch` to `vendor/ryujinx` and publishes `totk_xlate` with NativeAOT into
`games/totk/prebuilt/<rid>/`, which is ignored: only the macos-arm64 build is committed.

## RomFS

Tears of the Kingdom 1.0.0 needs no hactool:

```
python games/totk/scripts/extract_exefs.py <card.xci> --keys <prod.keys> --output <work>/extracted/v0
python games/totk/scripts/extract_romfs.py --keys <prod.keys> --manifest <work>/extracted/extraction-manifest.json --output <work>/extracted/romfs
```

Breath of the Wild merges the base RomFS with the 1.6.0 update through hactool's BKTR support. The project's hactool
hooks (title keys from a private file, the base NCA read in place) are macOS-only, so on Linux or WSL use a stock
hactool at the pinned commit with `--base-copy`; `--update-nca` picks 1.6.0 on a card that carries two updates:

```
python3 games/botw/scripts/extract_romfs.py <card.xci> --keys <prod.keys> --hactool <hactool> --output <work>/extracted/romfs \
    --update-nca <1.6.0 update NCA name> --base-copy
```

Both check the result against the version's expected RomFS SHA-256 in their `index.json`.

## Every shader, with what it needs

```
python games/totk/scripts/export_shader_facts.py --romfs <work>/extracted/romfs --output <work>/shader-facts \
    --translator games/totk/prebuilt/win-x64/totk_xlate.exe --spirv-cross <spirv-cross> [--game botw]
```

Unlike `prepare_shader_seed.py`, which translates the startup shaders and the listed bind states for the runtime, this
translates the whole inventory once for the default bind state and keeps each shader's SPIR-V and reflection.
`facts.jsonl` has one line per shader (its queried texture and image slots, vertex inputs read, colour outputs written,
translator notes, driver-bank reads and failures) and `summary.json` the totals.
