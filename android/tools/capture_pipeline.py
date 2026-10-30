#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Capture a bounded native BOTW Litep + non-root scheduler/display sidecar."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys
import time
from types import SimpleNamespace
from validate_scene import ROOT, PACKAGE, SERVICE, scene_sample
sys.path.insert(0, str(ROOT / 'porting/tools'))
from profiler import Target


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--serial', required=True, choices=['6edb0593'])
    p.add_argument('--expected-pid', required=True)
    p.add_argument('--expected-apk-sha256', required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=False)
    t = Target(SimpleNamespace(adb='adb', serial=a.serial, package=PACKAGE,
                              service=SERVICE, process_suffix=':game', expected_pid=a.expected_pid))
    report = dict(state='starting', serial=a.serial, pid=a.expected_pid,
                  clocks='PROF/Perfetto alignment requires recorded clock anchors; no wall-time inference')

    def save(name, value):
        (a.output / name).write_text(json.dumps(value, indent=2) + '\n')

    def c(*args):
        for attempt in range(80):
            try:
                return t.control(args)
            except ValueError as e:
                if str(e) != 'emulation_session_busy' or attempt == 79:
                    raise
                time.sleep(.025)

    def raw(name, *args):
        for attempt in range(80):
            result = t.adb('shell', *args)
            if 'error=emulation_session_busy' not in result:
                break
            if attempt == 79:
                raise RuntimeError('Busy diagnostic snapshot: ' + name)
            time.sleep(.025)
        (a.output / name).write_text(result)
        return result

    assert t.pid() == a.expected_pid
    assert t.adb('shell', 'getprop', 'ro.product.model').strip() == 'Odin3'
    apk = t.adb('shell', 'pm', 'path', PACKAGE).strip()
    assert apk.startswith('package:/data/app/') and '\n' not in apk
    assert t.adb('shell', 'sha256sum', apk[8:]).split()[0] == a.expected_apk_sha256
    modules = raw('guest-modules.txt', 'dumpsys', 'activity', 'service', PACKAGE + '/' + SERVICE, 'guest_modules')
    identity = json.loads(next(line.strip() for line in modules.splitlines() if line.strip().startswith('{')))
    assert str(identity['process_id']) == a.expected_pid
    assert next(m['build_id'] for m in identity['modules'] if m['name'] == 'main') == '8e9978d50bdd20b4c8395a106c27ffde00000000000000000000000000000000'
    assert scene_sample(c('autoload_status'), c('renderer_status'))
    assert c('renderer_publication', 'status')['publication_queue'] == '1'
    assert c('renderer_invalidate', 'status')['invalidate_fast'] == '1'
    assert c('renderer_schedule', 'status')['ordered_translation'] == '0'
    for command, field in [('profiler_ring', 'recording'), ('profiler_tags', 'tags'),
                           ('profiler_guest', 'guest'), ('profiler_address', 'address'),
                           ('profiler_hle', 'hle'), ('profiler_detail', 'detail')]:
        assert c(command, 'status')[field] == '0'
    assert c('profiler_capture', 'status')['capture_active'] == '0'
    lane_before = c('lane_status', 'status')['lane_enabled']
    report['configuration'] = c('renderer_schedule', 'status')
    raw('display.txt', 'dumpsys', 'display')
    raw('thermal-before.txt', 'dumpsys', 'thermalservice')
    raw('threads-before.txt', 'dumpsys', 'activity', 'service', PACKAGE + '/' + SERVICE, 'threads')
    layers = raw('layers.txt', 'dumpsys', 'SurfaceFlinger', '--list').splitlines()
    candidates = [s.removeprefix('RequestedLayerState{').split(' parentId=')[0].rstrip('}')
                  for s in layers if 'SurfaceView' in s and PACKAGE in s and 'GameActivity' in s and '(BLAST)#' in s]
    # Never substitute another application's layer or global display timestamps.
    report['surface_layer'] = candidates[0] if len(candidates) == 1 else None
    report['surface_layer_candidates'] = candidates
    config = '''buffers { size_kb: 65536 fill_policy: RING_BUFFER }
duration_ms: 10000
data_sources { config { name: "linux.ftrace" ftrace_config {
 ftrace_events: "sched/sched_switch"
 ftrace_events: "sched/sched_wakeup"
 ftrace_events: "sched/sched_waking"
 ftrace_events: "power/cpu_frequency"
 ftrace_events: "power/cpu_idle"
 atrace_apps: "org.macnative.android.botw"
} } }
data_sources { config { name: "linux.process_stats" process_stats_config { scan_all_processes_on_start: true } } }
'''
    (a.output / 'perfetto-config.pbtxt').write_text(config)
    remote = '/data/misc/perfetto-traces/mng-s1-' + a.expected_pid + '-' + str(time.time_ns()) + '.pftrace'
    capture_id = None
    scheduler = None
    try:
        c('lane_status', 'on'); c('lane_status', 'reset')
        raw('lanes-before.txt', 'dumpsys', 'activity', 'service', PACKAGE + '/' + SERVICE, 'lane_status', 'status')
        with (a.output / 'perfetto.log').open('w') as log:
            scheduler = subprocess.Popen(['adb', '-s', a.serial, 'shell', 'perfetto', '--txt', '-c', '-', '-o', remote],
                                         stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, text=True)
            scheduler.stdin.write(config); scheduler.stdin.close()
            time.sleep(.5)
            for command in ['profiler_tags', 'profiler_guest', 'profiler_address']:
                c(command, 'start')
            capture_id = c('profiler_capture', 'file', '512', '8')['capture_id']
            deadline = time.monotonic() + 35
            samples = []
            while time.monotonic() < deadline:
                assert t.pid() == a.expected_pid
                sample = dict(host_time=time.time(), renderer=c('renderer_status'))
                assert scene_sample({'stop_reason': '1'}, sample['renderer'])
                if report['surface_layer']:
                    sample['surface_latency'] = t.adb('shell', 'dumpsys', 'SurfaceFlinger', '--latency', shlex.quote(report['surface_layer']))
                samples.append(sample)
                save('samples.json', samples)
                state = c('profiler_capture', 'status')
                assert state['capture_id'] == capture_id
                if state['capture_state'] == 'ready':
                    break
                if state['capture_state'] == 'failed':
                    raise RuntimeError(str(state))
                time.sleep(.6)
            else:
                raise RuntimeError('Litep finalize timeout')
            scheduler.wait(timeout=20)
            report['perfetto_exit'] = scheduler.returncode
        report['litep'] = t.collect('file', capture_id, a.output / 'litep')
        capture_id = None
        raw('lanes-after.txt', 'dumpsys', 'activity', 'service', PACKAGE + '/' + SERVICE, 'lane_status', 'status')
        raw('threads-after.txt', 'dumpsys', 'activity', 'service', PACKAGE + '/' + SERVICE, 'threads')
        if scheduler.returncode == 0:
            subprocess.run(['adb', '-s', a.serial, 'pull', remote, str(a.output / 'scheduler.pftrace')], check=True)
            report['perfetto_sha256'] = hashlib.sha256((a.output / 'scheduler.pftrace').read_bytes()).hexdigest()
        raw('thermal-after.txt', 'dumpsys', 'thermalservice')
        report['state'] = 'captured'
    except Exception as error:
        report.update(state='failed', error=str(error))
        raise
    finally:
        if scheduler and scheduler.poll() is None:
            # The device-side session is independently bounded to ten seconds.
            scheduler.wait(timeout=15)
        if t.pid() == a.expected_pid:
            if capture_id:
                c('profiler_capture', 'stop')
            for command in ['profiler_tags', 'profiler_guest', 'profiler_address', 'profiler_ring']:
                c(command, 'stop')
            c('lane_status', 'on' if lane_before == '1' else 'off')
        save('result.json', report)


if __name__ == '__main__':
    main()
