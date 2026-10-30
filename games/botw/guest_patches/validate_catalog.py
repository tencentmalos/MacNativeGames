#!/usr/bin/env python3
"""Validate BOTW patch ownership and the approved Android recipe; never apply code."""
import argparse
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
CATALOG = Path(__file__).with_name('catalog.json')


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def build_id(value):
    require(isinstance(value, str) and re.fullmatch(r'(?:[0-9a-fA-F]{2}){1,32}', value), 'Invalid Build ID')
    raw = bytes.fromhex(value).rstrip(b'\0')
    require(bool(raw), 'Empty Build ID')
    return raw


def source(root, name):
    require(isinstance(name, str), 'Missing source path')
    path = (root / name).resolve()
    require(path.is_relative_to(root.resolve()) and path.is_file(), 'Source outside repository or missing')
    return path


def validate(catalog, root=ROOT, manifest=None):
    require(catalog['schema_version'] == 2 and catalog['kind'] == 'guest-patch-inventory', 'Unsupported inventory')
    require(catalog['profile'] == 'botw160', 'Unsupported title/version')
    require(catalog['source_manifest'] == 'games/botw/assets/expected-v1.6.0.json', 'Wrong source manifest')
    pins = json.loads(source(root, catalog['source_manifest']).read_text())
    require(catalog['title_id'] == pins['title_id'] and catalog['display_version'] == pins['display_version'], 'Title identity drift')
    require(set(catalog['modules']) == {'main', 'subsdk0', 'sdk'}, 'Missing module identity')
    for name, module in catalog['modules'].items():
        build_id(module['build_id'])
        require(module['source_sha256'] == pins['exefs_sha256'][name], 'Source identity drift')
    require(catalog['shared_defaults'] == {'macos-arm64': [], 'android-arm64': ['botw160.exclusive-acquire', 'botw160.realtime-vfr', 'botw160.pad-repeat', 'botw160.movie-pacing']}, 'Unreviewed default recipe')
    entries = {p['id']: p for p in catalog['patches']}
    require(entries and len(entries) == len(catalog['patches']), 'Duplicate/empty inventory')
    for name, entry in entries.items():
        require(name.startswith('botw160.') and entry['module'] in catalog['modules'], 'Wrong patch owner')
        require(entry['category'] in ('compatibility', 'feature', 'diagnostic'), 'Unknown category')
        require(entry['effect'] in ('guest_instructions', 'guest_state', 'host_policy'), 'Unknown effect')
        if name == 'botw160.exclusive-acquire':
            recipe = entry['shared_recipe']
            require(recipe == dict(path='games/botw/guest_patches/exclusive_acquire.cpp',
                entrypoint='apply_exclusive_acquire', platform='android-arm64',
                input_sha256='cf5d57a21cd85a627de7c9c500957d971b81b6aebbc3360ed78edbe350b41884',
                output_sha256='60474b0e3056daf4aee06c875de30bb001310d934437da0c4e99bdb76ca05007',
                text_sites=18249, rewrite_sites=63), 'Android recipe identity drift')
            body = source(root, recipe['path']).read_text()
            require(all(recipe[field] in body for field in ('entrypoint', 'input_sha256', 'output_sha256')),
                    'Recipe source identity mismatch')
        elif name == 'botw160.render-1080':
            recipe = entry['shared_recipe']
            require(recipe == dict(path='games/botw/guest_patches/render_resolution.cpp',
                entrypoint='apply_render_1080',platform='android-arm64',
                input_sha256='60474b0e3056daf4aee06c875de30bb001310d934437da0c4e99bdb76ca05007',
                output_sha256='d48e4b5eb443a5a9f8f370101d4fd7182a36830a4b87efa5a6c2be9edff025ea',
                module_offset=0x1821670,bytes=12,activation='explicit_1080p_launch_setting'), 'Resolution recipe identity drift')
            body = source(root,recipe['path']).read_text()
            require(all(recipe[field] in body for field in ('entrypoint','input_sha256','output_sha256')), 'Resolution source identity mismatch')
        elif name in ('botw160.realtime-vfr','botw160.pad-repeat','botw160.movie-pacing'):
            recipe=entry['shared_recipe']
            require(recipe == dict(path='games/botw/guest_patches/realtime.cpp',
                entrypoint='RealtimeSession' if name.endswith('movie-pacing') else 'apply_realtime',
                platform='android-arm64',activation='default_60fps_with_measured_game_time',
                group='botw160.realtime-vfr+pad-repeat+movie-pacing'), 'Timing recipe identity drift')
            require(recipe['entrypoint'] in source(root,recipe['path']).read_text(), 'Missing timing implementation')
        else:
            require(entry['shared_recipe'] is None, 'Unreviewed executable recipe')
        require(entry['reason'] and entry['evidence'] and entry['migration_acceptance'], 'Missing rationale or acceptance')
        require(set(entry['status']) == {'macos-arm64', 'android-arm64'} and
                entry['status']['android-arm64'] == ('implemented_pending_gameplay_validation' if name in ('botw160.exclusive-acquire','botw160.render-1080','botw160.realtime-vfr','botw160.pad-repeat','botw160.movie-pacing') else 'pending_validation') and
                entry['status']['macos-arm64'] in ('legacy', 'apple_mobile_reference'), 'Unverified migration status')
        implementation = entry['legacy_implementation']
        body = source(root, implementation['path']).read_text()
        require(implementation['functions'], 'Missing legacy function')
        for function in implementation['functions']:
            require(re.search(r'\b' + re.escape(function) + r'\s*\(', body), 'Missing legacy function: '+function)
        for evidence in entry['evidence']:
            source(root, evidence['path'])
        for relation in ('dependencies', 'conflicts'):
            values = entry[relation]
            require(len(values) == len(set(values)) and all(v in entries and v != name for v in values), 'Invalid '+relation)
        for other in entry['conflicts']:
            require(name in entries[other]['conflicts'], 'Asymmetric conflict')
    visited, active = set(), set()

    def walk(name):
        require(name not in active, 'Dependency cycle')
        if name in visited:
            return
        active.add(name)
        for dep in entries[name]['dependencies']:
            walk(dep)
        active.remove(name)
        visited.add(name)

    for name in entries:
        walk(name)
    if manifest is not None:
        require(manifest['schema_version'] == 1 and manifest['title'] == catalog['profile'] and
                manifest['target'] in ('macos', 'android'), 'Manifest profile/platform mismatch')
        modules = {m['name']: m for m in manifest['modules']}
        require(len(manifest['modules']) == 3 and modules.keys() == catalog['modules'].keys(), 'Manifest module mismatch')
        for name, expected in catalog['modules'].items():
            require(build_id(modules[name]['build_id']) == build_id(expected['build_id']) and
                    modules[name]['source_sha256'] == expected['source_sha256'], 'Manifest executable mismatch')
    return dict(status='pass', scope='inventory_and_android_recipe', profile=catalog['profile'], entries=len(entries), enabled_recipes=4)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(json.loads(CATALOG.read_text()),
        manifest=json.loads(args.manifest.read_text()) if args.manifest else None), indent=2))
