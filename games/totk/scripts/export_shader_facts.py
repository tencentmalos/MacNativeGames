#!/usr/bin/env python3
"""Translate every shader in a verified local RomFS for the default bind state and export what each one needs.

prepare_shader_seed.py translates the startup shaders and the listed bind states for the runtime. This exports the
whole inventory instead, for analysis by another runtime (a different NVN implementation, an emulator's direct NVN
frontend): every discovered shader goes through the same translator service with no answers, and its reflection
records the facts the Maxwell code alone could not give:

  deps            the texture and image slots the translator queried, the vertex inputs read, the colour outputs written
  notes           which queries fell back to a default (texture type 2D, image format R8G8B8A8, ...)
  drv_reads       the words of the driver constant banks c0 / c2 the shader reads
  drv_unfillable  driver-bank reads outside the ranges this project's replay fills (unknown driver constants)

Outputs, per game, in --output (game-derived data: keep it in the work folder, never in a repository):
  <variant>.spv, <variant>_b.json    SPIR-V and reflection of each translated shader (--keep-msl keeps the MSL too)
  facts.jsonl                         one line per discovered shader, failures included
  summary.json                        totals and distributions

  export_shader_facts.py --romfs <RomFS folder> --output <folder> --translator <totk_xlate> --spirv-cross <spirv-cross>
                         [--game totk|botw] [--modules rewritten/<version>] [--limit N] [--keep-msl]
"""
import argparse
import collections
import concurrent.futures
import json
import os
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import prepare_shader_seed as seed   # noqa: E402

STAGE_NAMES = {'vs': 'vertex', 'ps': 'fragment', 'cs': 'compute'}


def request_for(key, stage, control, code, out):
    """The request prepare_shader_seed.py sends for the default bind state, with the code written next to it."""
    variant = f'{key}-{seed.SUFFIX}'
    c1, length = struct.unpack_from('<II', control, 0x700)
    code_path = out/(variant+'.code')
    code_path.write_bytes(code[0 if stage == 'cs' else 0x30:length])
    request = dict(variant=variant, stage=stage, code=str(code_path.resolve()), c1=code[c1:length].hex(),
                   depth_minus_w=True, **seed.ANSWERS)
    if stage == 'cs':
        request['local'] = list(struct.unpack_from('<III', control, 0x748))
    return variant, code_path, request


def fact(key, stage, source, refl=None, error=None):
    row = dict(key=key, stage=stage, source=source)
    if error is not None:
        row['error'] = error
        return row
    deps = refl.get('deps', {})
    bindings = refl.get('bindings', {})
    row.update(
        tex_slots=sorted(int(s) for s in deps.get('tex', {})),
        img_slots=sorted(int(s) for s in deps.get('img', {})),
        attr_locations=sorted(int(s) for s in deps.get('attr', {})),
        fout_targets=sorted(int(s) for s in deps.get('fout', {})),
        threadgroup_size=refl.get('threadgroup_size'),
        notes=refl.get('notes', []),
        drv_reads=refl.get('drv_reads', {}),
        drv_unfillable=refl.get('drv_unfillable', []),
        softfloat64=refl.get('softfloat64', False),
        binding_kinds=dict(collections.Counter(b.get('kind', '?') for b in bindings)) if isinstance(bindings, list) else {},
    )
    return row


def note_class(note):
    """Group translator notes by what they defaulted, dropping the slot and handle numbers."""
    if '无 PC 类型' in note:
        return 'texture type unknown -> 2D'
    if '无 PC 格式' in note:
        return 'image format unknown -> R8G8B8A8Unorm'
    if 'c1 读越界' in note:
        return 'c1 read out of range'
    return note.split('：')[0][:60]


def summarize(rows, seconds, args):
    ok = [r for r in rows if 'error' not in r]
    failed = [r for r in rows if 'error' in r]
    def hist(values):
        return dict(sorted(collections.Counter(values).items()))
    vs = [r for r in ok if r['stage'] == 'vs']
    ps = [r for r in ok if r['stage'] == 'ps']
    # The facts the Maxwell code does not carry, as a runtime must supply them at bind time. Texture types are mostly
    # encoded in the instructions, so only queried ones count; every vertex input read and every colour output written
    # needs its component type; every queried image slot needs its format.
    facts_needed = dict(
        vertex_input_types=dict(shaders=sum(1 for r in vs if r['attr_locations']), inputs=sum(len(r['attr_locations']) for r in vs)),
        colour_output_types=dict(shaders=sum(1 for r in ps if r['fout_targets']), outputs=sum(len(r['fout_targets']) for r in ps)),
        image_formats=dict(shaders=sum(1 for r in ok if r['img_slots']), slots=sum(len(r['img_slots']) for r in ok)),
        queried_texture_types=dict(shaders=sum(1 for r in ok if any('无 PC 类型' in n for n in r['notes'])),
                                   queries=sum(1 for r in ok for n in r['notes'] if '无 PC 类型' in n)),
        compute_threadgroup=dict(shaders=sum(1 for r in ok if r['stage'] == 'cs')),
    )
    summary = dict(
        game=args.game, romfs=str(args.romfs), translator_sha256=seed.digest(args.translator.read_bytes()),
        spirv_cross_sha256=seed.digest(args.spirv_cross.read_bytes()),
        chain=seed.CHAIN, seconds=round(seconds, 1), facts_needed=facts_needed,
        discovered=len(rows), translated=len(ok), failed=len(failed),
        by_stage={STAGE_NAMES.get(s, s): dict(discovered=sum(1 for r in rows if r['stage'] == s),
                                              translated=sum(1 for r in ok if r['stage'] == s))
                  for s in sorted({r['stage'] for r in rows})},
        failures=dict(collections.Counter(r['error'].split(':')[0][:80] for r in failed).most_common(20)),
        texture_slots_per_shader=hist(len(r['tex_slots']) for r in ok),
        image_slots_per_shader=hist(len(r['img_slots']) for r in ok),
        vertex_inputs_per_vertex_shader=hist(len(r['attr_locations']) for r in ok if r['stage'] == 'vs'),
        colour_outputs_per_fragment_shader=hist(len(r['fout_targets']) for r in ok if r['stage'] == 'ps'),
        compute_threadgroup_sizes=dict(collections.Counter(
            'x'.join(map(str, r['threadgroup_size'])) for r in ok if r['stage'] == 'cs' and r['threadgroup_size']
        ).most_common(20)),
        defaulted_facts=dict(collections.Counter(note_class(n) for r in ok for n in r['notes']).most_common(20)),
        shaders_with_defaulted_facts=sum(1 for r in ok if r['notes']),
        driver_reads_unfillable=dict(collections.Counter(u for r in ok for u in r['drv_unfillable']).most_common(40)),
        shaders_with_unfillable_driver_reads=sum(1 for r in ok if r['drv_unfillable']),
        shaders_reading_driver_banks=dict(c0=sum(1 for r in ok if '0' in r['drv_reads']), c2=sum(1 for r in ok if '2' in r['drv_reads'])),
        binding_kinds=dict(collections.Counter(k for r in ok for k, n in r['binding_kinds'].items() for _ in range(n))),
        softfloat64_shaders=sum(1 for r in ok if r['softfloat64']),
        sources=dict(collections.Counter(r['source'].split('/')[0] for r in rows).most_common(20)),
    )
    return summary


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    for name in ['romfs', 'output', 'translator', 'spirv-cross']:
        p.add_argument('--'+name, type=Path, required=True)
    p.add_argument('--game', choices=sorted(seed.GAMES), default='totk')
    p.add_argument('--modules', type=Path, help='rewritten/<version>: shaders the game modules hold')
    p.add_argument('--limit', type=int, help='translate only the first N shaders (by key), for a quick check')
    p.add_argument('--keep-msl', action='store_true', help='keep the translator service\'s MSL and .res.json too')
    p.add_argument('--workers', type=int, default=min(8, os.cpu_count() or 4))
    args = p.parse_args()

    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    _, shaders, startup, archives = seed.inventory(args.romfs.resolve(), args.modules.resolve() if args.modules else None, args.game)
    keys = sorted(shaders)
    if args.limit:
        keys = keys[:args.limit]
    print(f'{len(shaders)} shaders discovered ({len(startup)} startup), translating {len(keys)} with {args.workers} services',
          flush=True)

    rows = {}
    lock = threading.Lock()
    pending = iter(keys)

    def worker():
        process = subprocess.Popen([str(args.translator.resolve()), '--service', '--spirv-cross', str(args.spirv_cross.resolve()),
                                    '--out', str(out), '--chain', seed.CHAIN],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        try:
            while True:
                with lock:
                    key = next(pending, None)
                if key is None:
                    return
                stage, control, code, source = shaders[key]
                try:
                    variant, code_path, request = request_for(key, stage, control, code, out)
                except (struct.error, ValueError) as e:
                    row = fact(key, stage, source, error=f'request: {e}')
                else:
                    process.stdin.write(json.dumps(request)+'\n'); process.stdin.flush()
                    response = process.stdout.readline()
                    if not response:
                        raise RuntimeError('translator service exited unexpectedly')
                    reply = json.loads(response)
                    code_path.unlink(missing_ok=True)
                    if reply.get('ok'):
                        row = fact(key, stage, source, json.loads((out/(variant+'_b.json')).read_text(encoding='utf-8')))
                        if not args.keep_msl:
                            for name in (variant+'_b.metal', variant+'.res.json'):
                                (out/name).unlink(missing_ok=True)
                    else:
                        row = fact(key, stage, source, error=str(reply.get('error')))
                        for name in (variant+'_b.metal', variant+'.res.json', variant+'.spv', variant+'_b.json'):
                            (out/name).unlink(missing_ok=True)
                with lock:
                    rows[key] = row
                    if len(rows) % 500 == 0:
                        bad = sum(1 for r in rows.values() if 'error' in r)
                        print(f'{len(rows)}/{len(keys)} done, {bad} failed; {time.monotonic()-started:.0f}s', flush=True)
        finally:
            process.stdin.close(); process.wait()

    with concurrent.futures.ThreadPoolExecutor(args.workers) as pool:
        for future in [pool.submit(worker) for _ in range(args.workers)]:
            future.result()

    ordered = [rows[k] for k in keys]
    with (out/'facts.jsonl').open('w', encoding='utf-8') as f:
        for row in ordered:
            f.write(json.dumps(row, ensure_ascii=False, separators=(',', ':'))+'\n')
    summary = summarize(ordered, time.monotonic()-started, args)
    summary['archives'] = len(archives)
    (out/'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print(f"translated {summary['translated']}/{summary['discovered']}, failed {summary['failed']}; "
          f"{summary['shaders_with_defaulted_facts']} with defaulted facts, "
          f"{summary['shaders_with_unfillable_driver_reads']} with unfillable driver reads; {summary['seconds']}s", flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
