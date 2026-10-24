"""Compile and run the shared native-port core without any game inputs."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which('cmake'), 'CMake required for native port core')
class PortRuntime(unittest.TestCase):
    def test_native_core(self):
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            commands = [
                ['cmake', '-S', str(root/'porting'), '-B', directory, '-DCMAKE_BUILD_TYPE=Debug'],
                ['cmake', '--build', directory, '-j', '2'],
                ['ctest', '--test-dir', directory, '--output-on-failure'],
            ]
            for command in commands:
                result = subprocess.run(command, capture_output=True, text=True, timeout=120)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
