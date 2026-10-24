# BOTW guest patch ownership

`catalog.json` owns the BOTW **1.6.0** adaptation inventory. It records executable
identity, rationale, compatibility/feature/diagnostic category, dependencies,
conflicts, legacy source evidence and host-specific acceptance requirements.
It includes title-specific runtime policies and state hooks as well as code
patches; a host callback is not converted into an instruction patch merely to
fit the executor.

The existing implementation still lives in `games/totk/runtime` for historical
build reuse. The `GAME_BOTW160` branches are BOTW code, not TOTK patch recipes.
BOTW 1.9.0, Wii U BOTW and TOTK are outside this profile. Their addresses and
layouts must not enter a 1.6.0 recipe.

| Owner | Responsibility |
| --- | --- |
| This game directory | Executable pins, semantic intent, verified selectors/byte guards, dependencies, feature defaults, state callbacks and acceptance evidence |
| Shared `porting/runtime` | Identity/preflight checks, atomic staging writes, bounded receipts and native-debugger descriptor publication |
| Host adapters | Memory protection, host thread/context access, platform ABI and device/backend capabilities |
| Foundation | Existing input/audio and command transport; no BOTW offsets or gameplay policy |

All entries are currently **migration inventory**, with `shared_recipe: null`.
Neither loader consumes this catalog as executable patches. Empty shared
defaults do not turn off the old Mac implementation: Mac behavior remains under
its existing guarded code and controls. Android has no enabled entry from this
inventory. The presence of a legacy verifier is evidence of an available test,
not a claim that the Android recipe passes it.

For each migration, first recover the exact original bytes and rewrite overlap
from private, hash-verified inputs. Generate a host-specific recipe with the
original Build ID/source hash and rewrite hash, then validate its byte guards
and behavior. Keep base-dependent fixups and state-page ownership explicit.
The current executor handles only static instruction writes to staging memory;
relocation-aware fixups and runtime state hooks need separate contracts.
Mac's relocated live image must not be passed off as the original rewrite.

The title planner must resolve dependencies and conflicts before submitting a
single transaction. For example, pad repeat depends on the VFR timing page;
the render-size and native-output paths are alternatives; pad diagnostics can
share a site with the input patch and must produce one composed, guarded write.
The executor rejects overlapping writes rather than silently ordering them.
Preserve private original bytes locally; receipts can record byte digests.

Use the native debugger's shared identity/session protocol for observations,
but keep BOTW hook locations, displaced instructions, object interpretation and
trap handling in this directory. Full source maps, guest stack reconstruction
and Android diagnostic-hook migration remain pending. Never ignore all traps
to make an unverified hook work.

```sh
python3 games/botw/guest_patches/validate_catalog.py
python3 games/botw/guest_patches/validate_catalog.py --manifest /private/botw160/manifest.json
```

The validator checks identity drift, source references and the dependency graph.
The optional manifest check binds the inventory to original modules from a
specific host preparation; it does not validate or activate executable recipes.
See [the shared porting contract](../../../porting/README.md) and
[validation evidence](../../../porting/VALIDATION.md).
