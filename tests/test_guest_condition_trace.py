"""Exercise the production condition path without running Guest code."""
from pathlib import Path
import platform
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(sys.platform == "darwin" and platform.machine() == "arm64", "native Apple scheduler")
class GuestConditionTrace(unittest.TestCase):
    def test_notification_timeout_and_disabled_markers(self):
        with tempfile.TemporaryDirectory() as folder:
            binary = str(Path(folder) / "condition-trace")
            build = subprocess.run([
                "xcrun", "clang", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-Wl,-dead_strip",
                "-I" + str(ROOT / "games/totk/third_party/d2r_gfx/gfx/nvn/include"),
                "-I" + str(ROOT / "porting/profiler"),
                str(ROOT / "tests/test_guest_condition_trace.c"), "-o", binary,
            ], capture_output=True, text=True, timeout=60)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([binary], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
