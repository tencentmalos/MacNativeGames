#!/usr/bin/env python3
"""Probe the MacNativeGames Android rewriter against verified Zelda modules.

This is a compatibility audit, not a game packager or rewrite correctness test.
All generated binaries and logs must remain outside Git repositories.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time


def digest(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def revision(path):
    return subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
    ).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--botw-exefs", required=True, type=Path)
    parser.add_argument("--totk-exefs", required=True, type=Path)
    parser.add_argument("--python", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    # Resolving a venv interpreter symlink loses its pyvenv.cfg and dependencies.
    interpreter = str(args.python.absolute())
    subprocess.run([interpreter, "-c", "import capstone, lz4.block"], check=True)
    output = args.output.resolve()
    if output.exists():
        parser.error("output must be a new directory; existing evidence is preserved")
    ancestor = output
    while not ancestor.exists():
        ancestor = ancestor.parent
    inside = subprocess.run(
        ["git", "-C", str(ancestor), "rev-parse", "--show-toplevel"],
        capture_output=True,
    )
    if inside.returncode == 0:
        parser.error("output must be outside a Git repository")
    repo = Path(__file__).resolve().parents[3]
    script = repo / "games/totk/third_party/d2r/patch_nso.py"
    profiles = [
        ("botw160", args.botw_exefs, "games/botw/assets/expected-v1.6.0.json"),
        ("totk100", args.totk_exefs, "games/totk/assets/expected-v1.0.0.json"),
    ]
    jobs = []
    # Verify every input before invoking any rewriter. No guessed patch sites.
    for title, exefs, manifest in profiles:
        expected_path = repo / manifest
        expected = json.loads(expected_path.read_text())
        for module in ("main", "sdk", "subsdk0"):
            source = (exefs / module).resolve()
            sha = digest(source)
            if sha != expected["exefs_sha256"][module]:
                parser.error(f"{title}/{module}: source SHA-256 mismatch")
            with source.open("rb") as handle:
                header = handle.read(256)
            if header[:4] != b"NSO0":
                parser.error(f"{title}/{module}: not an NSO")
            jobs.append(dict(title=title, module=module, source=str(source),
                             source_sha256=sha, build_id=header[0x40:0x60].hex(),
                             identity_manifest_sha256=digest(expected_path)))
    output.mkdir(parents=True, exist_ok=False)
    env = dict(os.environ, DM_TARGET="android", DM_RULESET="native", DM_PRIVATE_GATES="0")
    env.pop("DM_BOOTLIST", None)

    def run(job):
        destination = output / job["title"] / job["module"]
        destination.mkdir(parents=True)
        log = destination / "rewrite.log"
        started = time.monotonic()
        with log.open("w") as handle:
            result = subprocess.run(
                [interpreter, str(script), job["source"],
                 str(destination), job["module"]],
                env=env, stdout=handle, stderr=subprocess.STDOUT,
            )
        record = dict(job, returncode=result.returncode,
                      elapsed_seconds=round(time.monotonic() - started, 3),
                      log=str(log), log_tail=log.read_text(errors="replace").splitlines()[-12:])
        record["outputs"] = [
            dict(name=p.name, bytes=p.stat().st_size, sha256=digest(p))
            for p in sorted(destination.iterdir()) if p != log
        ]
        record["status"] = "rewrite_emitted_unvalidated" if result.returncode == 0 else "rewrite_failed"
        print(f'{job["title"]}/{job["module"]}: {record["status"]}', flush=True)
        return record

    with ThreadPoolExecutor(max_workers=2) as pool:
        records = list(pool.map(run, jobs))
    report = dict(
        schema_version=1, macnative_commit=revision(repo), rewriter_sha256=digest(script),
        call_window_sha256=digest(script.with_name("call_window.py")),
        target="android", ruleset="native", private_gates=False, modules=records,
        limitation="Emission does not prove semantic equivalence, Android execution, or game compatibility.",
    )
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return 0 if all(record["returncode"] == 0 for record in records) else 1


if __name__ == "__main__":
    raise SystemExit(main())
