#!/usr/bin/env python3
"""Package verified Android rewrites for the native loader; outputs stay outside Git.

Dynamic symbols/relocations follow scripts/prepare_runtime.py. No title addresses
are transplanted: the clock PLT is located from each input's own dynamic table.
"""
import argparse
import hashlib
import json
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'games/totk/third_party/d2r'))
from relr import decode_relr
from prepare_modules import digest


def metadata(image, layout):
    def read(fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(image):
            raise ValueError('Dynamic table access outside image')
        return struct.unpack_from(fmt, image, offset)

    mod0, = read('<I', 4)
    if image[mod0:mod0 + 4] != b'MOD0':
        raise ValueError('Missing MOD0')
    relative, = read('<i', mod0 + 4)
    tags = {}
    for offset in range(mod0 + relative, mod0 + relative + 65536, 16):
        tag, value = read('<qQ', offset)
        if tag == 0:
            break
        tags[tag] = value
    else:
        raise ValueError('Unterminated dynamic table')
    if tags.get(11) != 24 or tags.get(9) != 24 or tags.get(20) != 7:
        raise ValueError('Unsupported dynamic symbol/relocation format')
    if 4 not in tags:
        raise ValueError('This Android checkpoint requires the verified SysV hash table')
    nsym, = read('<I', tags[4] + 4)
    if not 0 < nsym <= 1000000 or not 0 <= tags[5] < tags[5] + tags[10] <= len(image):
        raise ValueError('Invalid symbol/string table size')
    symbols = []
    for i in range(nsym):
        no, info, other, section, value, size = read('<IBBHQQ', tags[6] + 24 * i)
        if no >= tags[10]:
            raise ValueError('Invalid symbol string offset')
        end = image.index(0, tags[5] + no, tags[5] + tags[10])
        symbols.append([image[tags[5] + no:end].decode(), value, size,
                        int(bool(section)), info >> 4, info & 15])
    relocs = [[where, 1027, 0, addend] for where, addend in
              decode_relr(image, tags, layout['DATA_OFF'], layout['KEY_PAGE_OFF'])]
    seen = {r[0] for r in relocs}
    for address, size in ((tags[7], tags[8]), (tags[23], tags[2])):
        if size % 24:
            raise ValueError('Invalid relocation size')
        for offset in range(address, address + size, 24):
            where, info, addend = read('<QQq', offset)
            kind, symbol = info & 0xffffffff, info >> 32
            if kind not in (257, 1025, 1026, 1027) or symbol >= nsym:
                raise ValueError('Unsupported relocation')
            if where % 8 or not layout['DATA_OFF'] <= where <= layout['KEY_PAGE_OFF'] - 8 or where in seen:
                raise ValueError('Invalid or duplicate relocation target')
            if kind == 1027 and not 0 <= addend < layout['KEY_PAGE_OFF']:
                raise ValueError('Invalid relative addend')
            seen.add(where)
            relocs.append([where, kind, symbol, addend])
    return dict(symbols=symbols, relocs=relocs)


def clock_plt(image, layout, module):
    target = '_ZN2nn2os13GetSystemTickEv'
    slots = [r[0] for r in module['relocs'] if r[1] == 1026 and module['symbols'][r[2]][0] == target]
    if len(slots) != 1:
        raise ValueError('Expected one original GetSystemTick import')
    matches = []
    for address in range(layout['TEXT_END'] - 65536, layout['TEXT_END'] - 12, 4):
        a, b, c, d = struct.unpack_from('<IIII', image, address)
        if a & 0x9f00001f != 0x90000010 or b & 0xffc003ff != 0xf9400211 or d != 0xd61f0220:
            continue
        imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
        imm = (imm ^ (1 << 20)) - (1 << 20)
        if (address & ~4095) + (imm << 12) + ((b >> 10) & 0xfff) * 8 == slots[0]:
            matches.append(address)
    if len(matches) != 1:
        raise ValueError(f'Expected one original clock PLT, found {len(matches)}')
    return dict(plt=matches[0], slot=slots[0], symbol=target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rewrite-report', type=Path, required=True)
    parser.add_argument('--title', choices=('botw160', 'totk100'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    if out.exists():
        parser.error('Output must be a new directory')
    import subprocess
    ancestor = out.parent
    while not ancestor.exists():
        ancestor = ancestor.parent
    if subprocess.run(['git', '-C', str(ancestor), 'rev-parse', '--show-toplevel'], capture_output=True).returncode == 0:
        parser.error('Private module packages must stay outside Git')
    report = json.loads(args.rewrite_report.read_text())
    rewriter = ROOT / 'games/totk/third_party/d2r/patch_nso.py'
    if report['target'] != 'android' or report['rewriter_sha256'] != digest(rewriter) or report['call_window_sha256'] != digest(rewriter.with_name('call_window.py')):
        parser.error('Rewrite target or generator identity mismatch')
    version = 'games/botw/assets/expected-v1.6.0.json' if args.title == 'botw160' else 'games/totk/assets/expected-v1.0.0.json'
    expected = json.loads((ROOT / version).read_text())['exefs_sha256']
    modules, copies = [], []
    for name in ('main', 'subsdk0', 'sdk'):
        records = [m for m in report['modules'] if m['title'] == args.title and m['module'] == name]
        if len(records) != 1 or records[0]['returncode']:
            parser.error(f'Missing successful rewrite: {name}')
        record = records[0]
        source = Path(record['source'])
        if record['source_sha256'] != expected[name] or digest(source) != expected[name]:
            parser.error(f'Original NSO identity mismatch: {name}')
        build_id = source.read_bytes()[0x40:0x60].hex()
        if build_id != record['build_id']:
            parser.error(f'Build ID mismatch: {name}')
        directory = Path(record['log']).parent
        for entry in record['outputs']:
            if digest(directory / entry['name']) != entry['sha256']:
                parser.error(f'Rewrite output was modified: {name}/{entry["name"]}')
        layout = {}
        for line in (directory / f'{name}.fixups.tsv').read_text().splitlines():
            _, key, value = line.split()
            layout[key] = int(value, 0)
        if layout.get('TARGET_ANDROID') != 1:
            parser.error('Apple rewrite rejected')
        path = directory / f'{name}.patched.bin'
        image = path.read_bytes()
        module = metadata(image, layout)
        module.update(name=name, file=path.name, file_size=len(image), layout=layout,
                      sha256=hashlib.sha256(image).hexdigest(), source_sha256=expected[name], build_id=build_id)
        gates = []
        for line in (directory / f'{name}.gates.tsv').read_text().splitlines():
            ret, kind, arg, original = line.split()
            if kind not in ('cntpct', 'cntvct', 'svc'):
                raise ValueError('Unsupported gate')
            gates.append([int(ret, 0), int(kind != 'svc'), int(arg), int(original, 0)])
        module['gates'] = gates
        if name == 'main':
            module['clock_probe'] = clock_plt(image, layout, module)
        modules.append(module)
        copies.append(path)
    package = dict(schema_version=1, target='android', title=args.title, modules=modules,
                   rewriter_sha256=report['rewriter_sha256'], call_window_sha256=report['call_window_sha256'])
    manifest = (json.dumps(package, separators=(',', ':')) + '\n').encode()
    profile = json.loads(Path(__file__).with_name('expected-modules.json').read_text())['profiles'][args.title]
    if hashlib.sha256(manifest).hexdigest() != profile['manifest_sha256']:
        parser.error('Package differs from the verified Android profile; review the generator and update its runtime pin together')
    out.mkdir(parents=True)
    for path in copies:
        shutil.copyfile(path, out / path.name)
    (out / 'manifest.json').write_bytes(manifest)
    print(json.dumps(dict(title=args.title, modules=len(modules), manifest_sha256=digest(out / 'manifest.json'), output=str(out))))


if __name__ == '__main__':
    main()
