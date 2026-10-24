"""Native port address proofs reject stale identity and unknown rewrite PCs."""
import copy
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('port_symbols', Path(__file__).resolve().parents[1]/'porting/tools/native_symbols.py')
symbols = importlib.util.module_from_spec(spec)
spec.loader.exec_module(symbols)


class PortSymbols(unittest.TestCase):
    def setUp(self):
        modules = [dict(name=n, build_id='aabb', source_sha256='a'*64, sha256='b'*64,
                       file_size=4096, layout=dict(TEXT_END=2048, PATCH_OFF=3072),
                       gates=[[3200, 0, 41, 128]], symbols=[['vendor_function', 120, 24, 1, 1, 2]])
                   for n in ('main', 'subsdk0', 'sdk')]
        self.manifest = json.dumps(dict(schema_version=1, title='botw160', target='android', modules=modules)).encode()
        self.snapshot = dict(schema_version=1, kind='mng-native-port', profile='botw160', platform='android-arm64',
            generation=7, manifest_sha256=hashlib.sha256(self.manifest).hexdigest(),
            modules=[dict(name=m['name'], build_id='aabb0000', source_sha256=m['source_sha256'], image_sha256=m['sha256'],
                          base=4096+i*8192, size=4096, text_size=2048, patch_offset=3072) for i, m in enumerate(modules)])

    def test_gate_and_vendor(self):
        r = symbols.resolve(self.snapshot, self.manifest, 4096+3200, 7)
        self.assertEqual((r['mapping'], r['module_offset'], r['vendor_symbol'], r['function_offset']),
                         ('exact-gate-return', 128, 'vendor_function', 8))
        self.assertTrue(r['live_target_and_stop_revalidation_required'])

    def test_unknown_is_not_nearest_symbol(self):
        for offset in (3204, 2500, 300):
            r = symbols.resolve(self.snapshot, self.manifest, 4096+offset, 7)
            self.assertIn('unknown_reason', r)
            self.assertFalse(r.get('vendor_symbol'))

    def test_stale_and_changed_identity(self):
        with self.assertRaises(ValueError): symbols.resolve(self.snapshot, self.manifest, 4224, 6)
        with self.assertRaises(ValueError): symbols.resolve(self.snapshot, self.manifest+b' ', 4224, 7)
        for field, value in [('build_id','ccdd'), ('image_sha256','c'*64), ('source_sha256','c'*64), ('size',2**64), ('patch_offset',3000)]:
            s = copy.deepcopy(self.snapshot)
            s['modules'][0][field] = value
            with self.assertRaises(ValueError): symbols.resolve(s, self.manifest, 4224, 7)
        s = copy.deepcopy(self.snapshot)
        s['modules'][1]['base'] = s['modules'][0]['base']
        with self.assertRaises(ValueError): symbols.resolve(s, self.manifest, 4224, 7)

    def test_platform_and_other_title_rejected(self):
        for key, value in [('platform','macos-arm64'), ('profile','totk100')]:
            s = self.snapshot | {key:value}
            with self.assertRaises(ValueError): symbols.resolve(s, self.manifest, 4224, 7)
