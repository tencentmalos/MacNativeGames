# SPDX-License-Identifier: MIT
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

path = Path(__file__).parents[2] / 'android/tools/benchmark_translation.py'
spec = importlib.util.spec_from_file_location('translation_benchmark', path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class FakeTarget:
    instances = []

    def __init__(self, args):
        self.args = args
        self.plan = '1'
        self.flush = '1'
        self.commands = []
        self.frame = 1000
        self.__class__.instances.append(self)

    def pid(self):
        return '42'

    def adb(self, *args, binary=False):
        if args[:3] == ('shell', 'dumpsys', 'window'):
            return 'mCurrentFocus=Window org.macnative.android.botw/org.macnative.android.GameActivity'
        if args[-1] == 'guest_modules':
            return json.dumps(dict(process_id=42, profile='botw160', modules=[dict(
                name='main', build_id='8e9978d50bdd20b4c8395a106c27ffde')]))
        if args[:3] == ('shell', 'pm', 'path'):
            return 'package:/data/app/example/base.apk'
        if args[:2] == ('shell', 'sha256sum'):
            return 'abc  /data/app/example/base.apk'
        if binary:
            return b'fake screenshot'
        return 'thermal status: 0'

    def control(self, command):
        self.commands.append(tuple(command))
        if command[0] == 'renderer_schedule':
            if command[1] == 'plans':
                self.plan = '1' if command[2] == 'on' else '0'
            if command[1] == 'flush':
                self.flush = '1' if command[2] == 'on' else '0'
            return dict(preparation_plan=self.plan, descriptor_plan='1',
                        image_read_fast='1', pipeline_early='1',
                        live_range_flush=self.flush, ordered_translation='0')
        if command[0] == 'renderer_status':
            self.frame += 25
            return dict(frame=str(self.frame), sample_age_ms='0', draws='3600',
                        dispatches='1', pipelines_created='0', shaders_loaded='0')
        if command[0] == 'profiler_detail':
            return dict(detail='0')
        if command[0] == 'profiler_ring':
            return dict(recording='0')
        return dict(capture_active='0', capture_id='1', capture_state='ready')

    def collect(self, mode, artifact_id, output):
        return dict(mode=mode, artifact_id=artifact_id)


class PreparationBenchmarkTests(unittest.TestCase):
    def test_flush_only_order_restore_and_failure(self):
        for fail in (False, True):
            class Target(FakeTarget):
                def control(self, command):
                    if fail and tuple(command[:2]) == ('profiler_capture', 'file'):
                        raise ValueError('synthetic capture failure')
                    return super().control(command)
            with self.subTest(failure=fail), tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / 'result'
                argv = ['benchmark', '--serial', 'test', '--output', str(output),
                        '--seconds', '5', '--capture-seconds', '1', '--flush-only']
                with patch.object(module, 'Target', Target), patch.object(module.sys, 'argv', argv), \
                     patch.object(module.time, 'sleep'), patch.object(module.time, 'monotonic', side_effect=iter(range(1000))), \
                     contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(module.main(), int(fail))
                report = json.loads((output / 'result.json').read_text())
                self.assertEqual(report['comparison'], 'live_range_flush')
                self.assertEqual(report['restored_configuration']['live_range_flush'], '1')
                self.assertEqual(report['restored_configuration']['preparation_plan'], '1')
                mutations = [c for c in Target.instances[-1].commands if c[0] == 'renderer_schedule' and c[1] != 'status']
                expected = ('off', 'on') if fail else ('off', 'on', 'off', 'on', 'on')
                self.assertEqual(mutations, [('renderer_schedule', 'flush', value) for value in expected])
                self.assertEqual(list(output.glob('*.png')), [])

    def test_plans_only_order_and_restore(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'result'
            argv = ['benchmark', '--serial', 'test', '--output', str(output),
                    '--seconds', '5', '--capture-seconds', '1', '--plans-only']
            with patch.object(module, 'Target', FakeTarget), patch.object(module.sys, 'argv', argv), \
                 patch.object(module.time, 'sleep'), patch.object(module.time, 'monotonic', side_effect=iter(range(1000))), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(module.main(), 0)
            report = json.loads((output / 'result.json').read_text())
            self.assertEqual(report['state'], 'ready')
            self.assertEqual(report['comparison'], 'preparation_plan')
            self.assertEqual([case['label'] for case in report['cases']], ['off_a', 'on_a', 'off_b', 'on_b'])
            self.assertEqual([case['configuration']['preparation_plan'] for case in report['cases']], ['0', '1', '0', '1'])
            mutations = [c for c in FakeTarget.instances[-1].commands if c[0] == 'renderer_schedule' and c[1] != 'status']
            self.assertEqual(mutations, [('renderer_schedule', 'plans', value) for value in ('off', 'on', 'off', 'on', 'on')])
            self.assertEqual(report['restored_configuration']['preparation_plan'], '1')
            self.assertEqual(len(list(output.glob('*.png'))), 4)

    def test_failure_restores_plan(self):
        class FailedCapture(FakeTarget):
            def control(self, command):
                if tuple(command[:2]) == ('profiler_capture', 'file'):
                    raise ValueError('synthetic capture failure')
                return super().control(command)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'result'
            argv = ['benchmark', '--serial', 'test', '--output', str(output), '--seconds', '5', '--plans-only']
            with patch.object(module, 'Target', FailedCapture), patch.object(module.sys, 'argv', argv), \
                 patch.object(module.time, 'sleep'), patch.object(module.time, 'monotonic', side_effect=iter(range(1000))), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(module.main(), 1)
            report = json.loads((output / 'result.json').read_text())
            self.assertEqual(report['state'], 'failed')
            self.assertEqual(report['error'], 'synthetic capture failure')
            self.assertEqual(report['restored_configuration']['preparation_plan'], '1')

    def test_comparison_modes_are_mutually_exclusive(self):
        argv = ['benchmark', '--serial', 'test', '--output', 'unused', '--plans-only', '--pipeline-only']
        with patch.object(module.sys, 'argv', argv), contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as raised:
                module.main()
        self.assertEqual(raised.exception.code, 2)


if __name__ == '__main__':
    unittest.main()
