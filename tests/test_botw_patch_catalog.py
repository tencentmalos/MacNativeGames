"""Keep the BOTW inventory bound to one title and prevent accidental activation."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('botw_catalog', ROOT / 'games/botw/guest_patches/validate_catalog.py')
catalog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalog)


class BotwPatchCatalog(unittest.TestCase):
    def setUp(self):
        self.data = json.loads(catalog.CATALOG.read_text())

    def test_inventory_and_manifest_identity(self):
        self.assertEqual(catalog.validate(self.data)['enabled_recipes'], 4)
        modules = [dict(name=n, **m) for n, m in self.data['modules'].items()]
        manifest = dict(schema_version=1, title='botw160', target='android', modules=modules)
        modules[0]['build_id'] = modules[0]['build_id'].upper() + '00'
        catalog.validate(self.data, manifest=manifest)
        modules[0]['build_id'] = 'aabb'
        with self.assertRaisesRegex(ValueError, 'executable mismatch'):
            catalog.validate(self.data, manifest=manifest)

    def test_wrong_version_or_pins(self):
        for field, value in [('profile', 'totk100'), ('display_version', '1.9.0')]:
            with self.assertRaises(ValueError):
                catalog.validate(self.data | {field: value})
        self.data['modules']['main']['source_sha256'] = 'a' * 64
        with self.assertRaisesRegex(ValueError, 'identity drift'):
            catalog.validate(self.data)

    def test_inventory_cannot_be_enabled_as_recipe(self):
        for mutate in (
            lambda d: d['shared_defaults']['android-arm64'].append('botw160.exclusive-acquire'),
            lambda d: d['patches'][0].update(shared_recipe='unverified.json'),
            lambda d: d['patches'][0]['status'].update({'android-arm64': 'ready'}),
        ):
            data = copy.deepcopy(self.data)
            mutate(data)
            with self.assertRaises(ValueError):
                catalog.validate(data)

    def test_dependency_cycle_and_conflict(self):
        self.data['patches'][0]['dependencies'] = [self.data['patches'][1]['id']]
        self.data['patches'][1]['dependencies'] = [self.data['patches'][0]['id']]
        with self.assertRaisesRegex(ValueError, 'cycle'):
            catalog.validate(self.data)
        self.data['patches'][1]['dependencies'] = []
        self.data['patches'][0]['conflicts'] = [self.data['patches'][1]['id']]
        with self.assertRaisesRegex(ValueError, 'Asymmetric'):
            catalog.validate(self.data)
