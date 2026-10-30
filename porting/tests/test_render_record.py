#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("record", Path(__file__).resolve().parents[1] / "tools/render_record.py")
record = importlib.util.module_from_spec(spec)
spec.loader.exec_module(record)


class Integrity(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.header = {"type": "header", "schema": "mng.nvn-index.v1", "run": "a" * 32, "request": 1,
                       "first_frame": 10, "frame_count": 1}
        self.action = {"type": "action", "frame": 10, "event": 1}
        self.footer = {"type": "footer", "complete": True, "events": 1, "completed_frames": 1}
        self.rows = [self.header, self.action, {"type": "present", "frame": 10}, self.footer]
        self.index = self.root / "commands.nvn.jsonl"

    def tearDown(self):
        self.temp.cleanup()

    def write(self):
        self.index.write_text("".join(json.dumps(row) + "\n" for row in self.rows))

    def test_complete_and_missing_footer(self):
        self.write(); self.assertEqual(record.inspect_index(self.index)[1]["events"], 1)
        self.rows.pop(); self.write()
        with self.assertRaisesRegex(RuntimeError, "incomplete"):
            record.inspect_index(self.index)

    def test_duplicate_action(self):
        self.rows.insert(2, dict(self.action)); self.write()
        with self.assertRaisesRegex(RuntimeError, "duplicate"):
            record.inspect_index(self.index)

    def test_partial(self):
        self.footer["complete"] = False; self.footer["reason"] = "event_limit"; self.write()
        with self.assertRaisesRegex(RuntimeError, "event_limit"):
            record.inspect_index(self.index)

    def test_wrong_frame(self):
        self.action["frame"] = 11; self.write()
        with self.assertRaisesRegex(RuntimeError, "frame"):
            record.inspect_index(self.index)

    def test_integrity_and_identity(self):
        self.write()
        rdc = self.root / "capture.rdc"; rdc.write_bytes(b"synthetic integrity fixture; not a replay capture")
        manifest = {"schema": record.SCHEMA, "run": self.header["run"], "request": 1,
                    "artifacts": [{"path": p.name, "sha256": record.digest(p), "size": p.stat().st_size} for p in (self.index, rdc)]}
        record.verify_bundle(self.root, manifest)
        manifest["run"] = "b" * 32
        with self.assertRaisesRegex(RuntimeError, "identity"):
            record.verify_bundle(self.root, manifest)
        manifest["run"] = self.header["run"]; rdc.write_bytes(b"changed")
        with self.assertRaisesRegex(RuntimeError, "integrity"):
            record.verify_bundle(self.root, manifest)

    def test_path_escape(self):
        with self.assertRaisesRegex(RuntimeError, "artifacts"):
            record.verify_bundle(self.root, {"schema": record.SCHEMA, "artifacts": [{"path": "../outside"}]})


if __name__ == "__main__":
    unittest.main()
