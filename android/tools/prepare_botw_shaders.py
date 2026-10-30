#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build BOTW Vulkan bundles from verified private inputs and Mac shader metadata.

No game bytes are included here. Outputs must stay outside the repository.
The pinned D2R MIT exporter supplies the XGS container and SPIR-V reflection.
"""
import argparse
import ast
import collections
import concurrent.futures
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zipfile

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "games/totk/scripts"))
import prepare_shader_seed as seed


def sha(data):
    return hashlib.sha256(data).hexdigest()


def exporter(source):
    """Load only the pinned pure helpers; dmsh_vk.py has a CLI at module scope."""
    path = source / "dm/gfx/tools/dmsh_vk.py"
    names = {"insts", "rewrite", "add_format_caps", "spirv_info", "image_desc", "decode_tex", "export"}
    functions = [n for n in ast.parse(path.read_text()).body if isinstance(n, ast.FunctionDef) and n.name in names]
    if {n.name for n in functions} != names:
        raise ValueError("Pinned exporter helpers changed")
    scope = dict(struct=struct, hashlib=hashlib, collections=collections,
                 STAGE_NO={"vs": 0, "hs": 1, "ds": 2, "gs": 3, "ps": 4, "cs": 5},
                 TEX_BASE=0x168, SAMP_BASE=0x568, IMG_BASE=0x120)
    exec(compile(ast.Module(body=functions, type_ignores=[]), str(path), "exec"), scope)
    separate = scope["decode_tex"]

    def decode(slots, handle, image):
        texture, sampler, kind = separate(slots, handle, image)
        # Same combined-handle table as MacNativeGames Service.cs DecodeTex.
        if texture is None and not image and (slots < 0 or slots & 0xffff == 2):
            words = [handle & 0x3fff] + ([(handle >> 14) & 0x3fff] if kind else [])
            for word in words:
                if 0x20 <= word * 4 < 0x120 and (word * 4 - 0x20) % 8 == 0:
                    texture = (word * 4 - 0x20) // 8
                    break
        return texture, sampler, kind

    scope["decode_tex"] = decode
    spec = importlib.util.spec_from_file_location("mng_xgs_pack", source / "dm/gfx/tools/pack_game_shaders.py")
    packer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(packer)
    return scope, packer.pack


def bundle_inventory(bundle, raw, results, jobs):
    """Package validated static modules and their actual Vulkan dependencies.

    Texture dimensions taken from instructions are not Vulkan specializations.
    Metal's depth-texture spelling is likewise not a SPIR-V variant dependency.
    Queried texture/image specializations remain unavailable until represented.
    """
    grouped = collections.defaultdict(list)
    for result in results:
        grouped[result["key"][:16]].append(result)
    files, unavailable = {}, []

    def publish(name, data):
        if name in files and files[name] != sha(data):
            raise ValueError("Truncated shader content identity collision: " + name)
        (bundle / name).write_bytes(data)
        files[name] = sha(data)

    for key, rows in sorted(grouped.items()):
        if not all(r["status"] == "pass" for r in rows):
            unavailable.append(key); continue
        data = [(bundle / (r["key"] + ".xgs")).read_bytes() for r in rows]
        if len({sha(b) for b in data}) == 1:
            publish(key + ".xgs", data[0]); continue
        records = []
        stage = jobs[rows[0]["key"]]["stage"]
        if stage not in ("vs", "ps"):
            unavailable.append(key); continue
        for row, packed in zip(rows, data):
            job = jobs[row["key"]]
            reflection = json.loads((raw / (row["key"] + ".res.json")).read_text())
            if reflection.get("tex_queried") or reflection.get("img_queried") or job["stage"] != stage:
                break
            meta = json.loads((bundle / (row["key"] + ".vk.json")).read_text())
            if stage == "vs":
                slots = sorted({i["location"] for i in meta["interfaces"] if i["storage"] == "input"})
                kind, deps = 0, job.get("attr", {})
            else:
                slots = [i for i in range(8) if (reflection["frag_out_map"] >> (4*i)) & 15]
                kind, deps = 1, job.get("fout", {})
            conditions = []
            for slot in slots:
                if slot >= (16 if stage == "vs" else 8):
                    raise ValueError("Unsupported shader interface slot")
                conditions.append(struct.pack("<III", kind, slot,
                    {"float": 0, "sint": 1, "uint": 2}[deps.get(str(slot), "float")]))
            content_key = sha(packed)[:16]
            records.append((content_key.encode() + struct.pack("<I", len(conditions)) + b"".join(conditions), content_key, packed))
        else:
            index = struct.pack("<IIII", 0x31564758, 1, 0 if stage == "vs" else 4, len(records))
            index += b"".join(r[0] for r in records)
            for _, content_key, packed in records:
                publish(content_key + ".xgs", packed)
            publish(key + ".xgv", index)
            continue
        unavailable.append(key)
    return files, unavailable


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for name in ("romfs", "modules", "mac-seed", "d2r", "output"):
        ap.add_argument("--" + name, type=Path, required=True)
    ap.add_argument("--translator", type=Path, default=REPO / "games/totk/prebuilt/macos-arm64/totk_xlate")
    ap.add_argument("--spirv-opt", default="spirv-opt")
    ap.add_argument("--spirv-val", default="spirv-val")
    ap.add_argument("--jobs", type=int, default=8)
    args = ap.parse_args()
    if args.output.resolve().is_relative_to(REPO) or not 1 <= args.jobs <= 32:
        ap.error("Use a private output directory outside Git and 1..32 workers")
    pin = json.loads((REPO / "android/dependencies.json").read_text())["d2r_vulkan"]["commit"]
    actual = subprocess.check_output(["git", "-C", str(args.d2r), "rev-parse", "HEAD"], text=True).strip()
    if actual != pin:
        ap.error("D2R checkout does not match dependencies.json")
    for tool in (args.spirv_opt, args.spirv_val):
        if not shutil.which(tool):
            ap.error("Missing tool: " + tool)
    args.output.mkdir(parents=True, exist_ok=False)
    raw, bundle = args.output / "translated", args.output / "bundle"
    raw.mkdir(); bundle.mkdir()
    _, inventory, _, _ = seed.inventory(args.romfs, args.modules, "botw")
    metas = collections.defaultdict(list)
    for path in sorted(args.mac_seed.glob("*_b.json")):
        metas[path.name[:64]].append(path)
    jobs, identities = [], {}
    for full, (stage, control, code, origin) in sorted(inventory.items()):
        if sha(control + code) != full or full[:16] in identities:
            raise ValueError("Shader identity mismatch or truncated key collision")
        key = full[:16]
        identities[key] = dict(sha256=full, source=origin, variants=[])
        path = args.output / (key + ".code")
        path.write_bytes(code[0 if stage == "cs" else 0x30:])
        c1, length = struct.unpack_from("<II", control, 0x700)
        if length != len(code) or c1 > length:
            raise ValueError("Invalid shader code extent")
        for meta_path in metas[full] or [None]:
            suffix = meta_path.stem.split("-")[-1] if meta_path else "default"
            meta = json.loads(meta_path.read_text()) if meta_path else dict(deps=dict(tex={}, img={}, attr={}, fout={}))
            job = dict(key=key + "-" + suffix, stage=stage, code=str(path), c1=code[c1:].hex(),
                       debug=True, reduced=True, depth_minus_w=True,
                       early_fragment_tests=bool(control[0x718]), **meta["deps"])
            if stage == "cs":
                job["local"] = list(struct.unpack_from("<III", control, 0x748))
            jobs.append(job)
            identities[key]["variants"].append(dict(key=job["key"], deps=meta["deps"],
                metadata_sha256=sha(meta_path.read_bytes()) if meta_path else None))
    inputs = dict(schema=1, translator_sha256=sha(args.translator.read_bytes()), d2r_commit=pin,
                  romfs_index_sha256=sha((args.romfs / "index.json").read_bytes()), identities=identities)
    (args.output / "inputs.json").write_text(json.dumps(inputs, indent=2))
    print(f"Translating {len(jobs)} variants of {len(identities)} shaders", flush=True)

    def translate(worker):
        path = args.output / f"jobs-{worker}.json"
        path.write_text(json.dumps(jobs[worker::args.jobs]))
        with (args.output / f"translate-{worker}.log").open("w") as log:
            return subprocess.run([str(args.translator), str(path), str(raw)], stdout=log, stderr=subprocess.STDOUT).returncode

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        translation_status = list(pool.map(translate, range(args.jobs)))
    helpers, pack = exporter(args.d2r)

    def validate(job):
        key = job["key"]
        try:
            rewritten = raw / (key + ".r.spv")
            rewritten.write_bytes(helpers["rewrite"]((raw / (key + ".spv")).read_bytes(), job["stage"]))
            final = bundle / (key + ".spv")
            subprocess.run([args.spirv_opt, "--trim-capabilities", str(rewritten), "-o", str(final)], check=True, capture_output=True)
            data = helpers["add_format_caps"](final.read_bytes()); final.write_bytes(data)
            subprocess.run([args.spirv_val, "--target-env", "vulkan1.1", str(final)], check=True, capture_output=True)
            reflection = helpers["export"](key, job["stage"], bytes.fromhex(job["c1"]), data,
                                           json.loads((raw / (key + ".res.json")).read_text()))
            path = bundle / (key + ".vk.json"); path.write_text(json.dumps(reflection))
            _, packed = pack(path); (bundle / (key + ".xgs")).write_bytes(packed)
            return dict(key=key, status="pass", sha256=sha(packed))
        except Exception as error:
            detail = error.stderr.decode(errors="replace") if isinstance(error, subprocess.CalledProcessError) else str(error)
            return dict(key=key, status="fail", reason=detail[:2000])

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(validate, jobs))
    files, unavailable = bundle_inventory(bundle, raw, results, {j["key"]: j for j in jobs})
    manifest = dict(schema=1, identity="sha256-control-code-prefix16", files=files,
                    translator_sha256=inputs["translator_sha256"], unavailable=unavailable)
    (bundle / "manifest.json").write_text(json.dumps(manifest, sort_keys=True))
    with zipfile.ZipFile(bundle / "bundle.zip", "w", compression=zipfile.ZIP_DEFLATED, compresslevel=1) as archive:
        for name in sorted(files):
            archive.write(bundle / name, arcname=name)
    (args.output / "validation.json").write_text(json.dumps(dict(translation_status=translation_status, results=results), indent=2))
    print(f"Validated {len(files)} bundles; {len(unavailable)} remain unavailable. No gameplay success implied.")
    return 1 if unavailable else 0


if __name__ == "__main__":
    sys.exit(main())
