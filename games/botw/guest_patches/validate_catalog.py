#!/usr/bin/env python3
"""Validate BOTW migration inventory; never apply or activate a patch."""
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
    require(catalog['schema_version'] == 1 and catalog['kind'] == 'guest-patch-inventory', 'Unsupported inventory')
    require(catalog['profile'] == 'botw160', 'Unsupported title/version')
    require(catalog['source_manifest'] == 'games/botw/assets/expected-v1.6.0.json', 'Wrong source manifest')
    pins = json.loads(source(root, catalog['source_manifest']).read_text())
    require(catalog['title_id'] == pins['title_id'] and catalog['display_version'] == pins['display_version'], 'Title identity drift')
    require(set(catalog['modules']) == {'main', 'subsdk0', 'sdk'}, 'Missing module identity')
    for name, module in catalog['modules'].items():
        build_id(module['build_id'])
        require(module['source_sha256'] == pins['exefs_sha256'][name], 'Source identity drift')
    require(catalog['shared_defaults'] == {'macos-arm64': [], 'android-arm64': []}, 'Inventory cannot enable patches')
    entries = {p['id']: p for p in catalog['patches']}
    require(entries and len(entries) == len(catalog['patches']), 'Duplicate/empty inventory')
    for name, entry in entries.items():
        require(name.startswith('botw160.') and entry['module'] in catalog['modules'], 'Wrong patch owner')
        require(entry['category'] in ('compatibility', 'feature', 'diagnostic'), 'Unknown category')
        require(entry['effect'] in ('guest_instructions', 'guest_state', 'host_policy'), 'Unknown effect')
        require(entry['shared_recipe'] is None, 'Executable recipes need a separate reviewed schema')
        require(entry['reason'] and entry['evidence'] and entry['migration_acceptance'], 'Missing rationale or acceptance')
        require(set(entry['status']) == {'macos-arm64', 'android-arm64'} and
                entry['status']['android-arm64'] == 'pending_validation' and
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
    return dict(status='pass', scope='inventory_only', profile=catalog['profile'], entries=len(entries), enabled_recipes=0)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(json.loads(CATALOG.read_text()),
        manifest=json.loads(args.manifest.read_text()) if args.manifest else None), indent=2))
