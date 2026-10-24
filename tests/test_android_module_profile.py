"""Public provenance pins must track the exact offline Android generator."""
import hashlib
import json
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AndroidModuleProfile(unittest.TestCase):
    def test_generator_and_source_identity(self):
        profiles = json.loads((ROOT / 'games/totk/android/expected-modules.json').read_text())['profiles']
        for title, version in [('botw160', 'games/botw/assets/expected-v1.6.0.json'),
                               ('totk100', 'games/totk/assets/expected-v1.0.0.json')]:
            profile = profiles[title]
            sources = json.loads((ROOT / version).read_text())['exefs_sha256']
            for key, file in [('rewriter_sha256', 'patch_nso.py'), ('call_window_sha256', 'call_window.py')]:
                actual = hashlib.sha256((ROOT / 'games/totk/third_party/d2r' / file).read_bytes()).hexdigest()
                self.assertEqual(profile[key], actual, 'Regenerate and validate Android profiles before updating their pins')
            self.assertEqual([m['name'] for m in profile['modules']], ['main', 'subsdk0', 'sdk'])
            for module in profile['modules']:
                self.assertEqual(sources[module['name']], module['source_sha256'])


if __name__ == '__main__':
    unittest.main()
