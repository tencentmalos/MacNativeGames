# SPDX-License-Identifier: MIT
import importlib.util
from argparse import Namespace
from pathlib import Path
import unittest
from unittest.mock import Mock

spec = importlib.util.spec_from_file_location("port_profiler", Path(__file__).parents[1]/"tools/profiler.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

class TargetTests(unittest.TestCase):
    def test_numbered_target_rows_are_preserved(self):
        self.assertEqual(module.fields("schema=1\ntarget_0=1280x720\n0invalid=no"),
                         {"schema": "1", "target_0": "1280x720"})
    def target(self):
        return module.Target(Namespace(serial="device-1",package="org.example.game",
                             service="org.example.DebugService",process_suffix=":game",expected_pid="42"))
    def test_exact_game_process(self):
        target=self.target();target.adb=Mock(side_effect=["42", "SERVICE org.example.game/org.example.DebugService pid=42\nprocess_id=42\ncapture_state=ready", "42"])
        self.assertEqual(target.control(["profiler_capture", "status"])["process_id"],"42")
    def test_guest_markers_use_the_same_exact_pid_transport(self):
        target=self.target();target.adb=Mock(side_effect=["42", "SERVICE org.example.game/org.example.DebugService pid=42\nprocess_id=42\nguest=1", "42"])
        self.assertEqual(target.control(["profiler_guest", "start"])["guest"],"1")
    def test_launcher_service_is_rejected(self):
        target=self.target();target.adb=Mock(side_effect=["42", "SERVICE org.example.game/org.example.DebugService pid=41\nprocess_id=41"])
        with self.assertRaises(ValueError): target.control(["profiler_ring", "status"])
    def test_restarted_process_is_rejected(self):
        target=self.target();target.adb=Mock(side_effect=["42", "SERVICE org.example.game/org.example.DebugService pid=42", "43"])
        with self.assertRaises(ValueError): target.control(["profiler_ring", "status"])
    def test_runtime_identity_must_match(self):
        target=self.target();target.adb=Mock(side_effect=["42", "SERVICE org.example.game/org.example.DebugService pid=42\nprocess_id=43", "42"])
        with self.assertRaises(ValueError): target.control(["profiler_ring", "status"])
    def test_argument_injection_is_rejected_before_adb(self):
        target=self.target();target.adb=Mock()
        with self.assertRaises(ValueError): target.control(["profiler_ring", "status;id"])
        target.adb.assert_not_called()
    def test_collection_requires_exact_ready_id(self):
        target=self.target();target.control=Mock(return_value={"capture_id":"2","capture_state":"ready"})
        with self.assertRaises(ValueError): target.collect("file","1","unused")
    def test_collection_rejects_external_path(self):
        target=self.target();target.control=Mock(return_value={"capture_id":"1","capture_state":"ready","capture_mode":"file","capture_active":"0","capture_path":"/sdcard/private.prof"})
        with self.assertRaises(ValueError): target.collect("file","1","unused")

if __name__=="__main__": unittest.main()
