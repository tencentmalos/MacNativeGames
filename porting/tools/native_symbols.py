#!/usr/bin/env python3
"""Resolve a captured native-port PC using its exact private metadata manifest.

Read-only/offline. The caller must retain the native-debugger target token,
session generation and stop epoch. A file is never authority to set a live
breakpoint; reread the descriptor and revalidate the target before mutation.
"""
import argparse
import hashlib
import json
from pathlib import Path


def build_id(value):
    if not isinstance(value, str) or not 2 <= len(value) <= 64 or len(value) % 2 or any(c not in '0123456789abcdefABCDEF' for c in value):
        raise ValueError('Invalid Build ID')
    raw = bytes.fromhex(value)
    if not raw.strip(b'\0'):
        raise ValueError('Empty Build ID')
    return raw.rstrip(b'\0').hex()


def integer(value, minimum=0, maximum=(1 << 64)-1):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError('Invalid integer/range')
    return value


def bind(snapshot, manifest_bytes, generation):
    if snapshot.get('schema_version') != 1 or snapshot.get('kind') != 'mng-native-port':
        raise ValueError('Unsupported snapshot')
    if integer(snapshot['generation'], 1) != integer(generation, 1):
        raise ValueError('Stale load generation')
    if hashlib.sha256(manifest_bytes).hexdigest() != snapshot['manifest_sha256']:
        raise ValueError('Manifest digest mismatch')
    manifest = json.loads(manifest_bytes)
    if snapshot['profile'] != 'botw160' or manifest['title'] != snapshot['profile']:
        raise ValueError('Only the BOTW 1.6.0 port is enabled')
    if snapshot['platform'] not in ('macos-arm64', 'android-arm64') or manifest['target']+'-arm64' != snapshot['platform']:
        raise ValueError('Host ABI mismatch')
    metadata = {m['name']: m for m in manifest['modules']}
    loaded = {m['name']: m for m in snapshot['modules']}
    if (set(metadata) != {'main', 'subsdk0', 'sdk'} or set(loaded) != set(metadata)
            or len(manifest['modules']) != 3 or len(snapshot['modules']) != 3):
        raise ValueError('Missing/duplicate module')
    ranges = []
    for name, live in loaded.items():
        meta = metadata[name]
        if (build_id(live['build_id']) != build_id(meta['build_id']) or
                live['source_sha256'] != meta['source_sha256'] or live['image_sha256'] != meta['sha256']):
            raise ValueError('Loaded executable identity mismatch')
        base, size = integer(live['base'], 1), integer(live['size'], 1)
        if size > (1 << 64)-base or size < meta['file_size']:
            raise ValueError('Invalid mapped extent')
        if live['text_size'] != meta['layout']['TEXT_END'] or live['patch_offset'] != meta['layout']['PATCH_OFF']:
            raise ValueError('Layout mismatch')
        if not 0 < live['text_size'] <= live['patch_offset'] < meta['file_size'] <= size:
            raise ValueError('Invalid segment extents')
        ranges.append((base, base+size))
    ranges.sort()
    if any(a[1] > b[0] for a, b in zip(ranges, ranges[1:])):
        raise ValueError('Overlapping loaded modules')
    return loaded, metadata


def resolve(snapshot, manifest_bytes, address, generation):
    loaded, metadata = bind(snapshot, manifest_bytes, generation)
    address = integer(address)
    result = dict(runtime_address=address, generation=generation, historical=True,
                  live_target_and_stop_revalidation_required=True)
    for name, live in loaded.items():
        if not live['base'] <= address < live['base']+live['size']:
            continue
        meta = metadata[name]
        offset = address-live['base']
        result.update(module=name, build_id=live['build_id'], host_module_offset=offset,
                      source_sha256=live['source_sha256'], image_sha256=live['image_sha256'])
        if offset < live['text_size']:
            logical = offset
            result['mapping'] = 'original-text-location-may-contain-rewrite-branch'
        elif offset >= live['patch_offset']:
            gates = [g for g in meta['gates'] if g[0] == offset]
            if len(gates) != 1:
                return result | dict(mapping='unknown', unknown_reason='No exact gate-return mapping; full rewrite source map required')
            logical = gates[0][3]
            integer(logical, 0, live['text_size']-1)
            result['mapping'] = 'exact-gate-return'
        else:
            return result | dict(mapping='unknown', unknown_reason='Data/key-page address, not guest code')
        result['module_offset'] = logical
        symbols = [s for s in meta['symbols'] if s[3] and s[5] == 2 and
                   (s[1] == logical or (s[2] > 0 and s[1] <= logical < s[1]+s[2]))]
        if symbols:
            symbols.sort(key=lambda s: (s[1] != logical, s[2], s[0]))
            symbol = symbols[0]
            result.update(vendor_symbol=symbol[0], function_offset=logical-symbol[1], confidence='vendor')
        else:
            result.update(vendor_symbol=None, unknown_reason='No containing vendor function; no nearest-symbol guess')
        return result
    return result | dict(mapping='unknown', unknown_reason='Outside registered guest modules; inspect native host symbols')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--snapshot', type=Path, required=True)
    p.add_argument('--manifest', type=Path, required=True)
    p.add_argument('--generation', type=int, required=True)
    p.add_argument('--address', type=lambda s: int(s, 0), required=True)
    a = p.parse_args()
    if a.snapshot.stat().st_size > 8 << 20 or a.manifest.stat().st_size > 64 << 20:
        p.error('Input exceeds diagnostic limit')
    print(json.dumps(resolve(json.loads(a.snapshot.read_bytes()), a.manifest.read_bytes(), a.address, a.generation), indent=2))


if __name__ == '__main__':
    main()
