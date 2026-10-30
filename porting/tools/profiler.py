#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Control and collect a Foundation profiler in an explicit Android process."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess


def fields(text):
    return {k: v for line in text.splitlines()
            for k, sep, v in [line.strip().partition('=')]
            if sep and re.fullmatch(r'[a-z_][a-z0-9_]*', k)}


class Target:
    def __init__(self, args):
        self.args = args
        for value, pattern in [(args.serial, r'[A-Za-z0-9_.:-]+'),
                               (args.package, r'[A-Za-z][A-Za-z0-9_.]+'),
                               (args.service, r'[A-Za-z][A-Za-z0-9_.]+'),
                               (args.process_suffix, r'(?::[A-Za-z0-9_]+)?')]:
            if not re.fullmatch(pattern, value):
                raise ValueError('Invalid target selector')

    def adb(self, *args, binary=False):
        return subprocess.check_output([self.args.adb, '-s', self.args.serial, *args],
                                       text=not binary, timeout=30)

    def pid(self):
        pid = self.adb('shell', 'pidof', self.args.package+self.args.process_suffix).strip()
        if not pid.isdecimal() or (self.args.expected_pid and pid != self.args.expected_pid):
            raise ValueError('Expected exactly one matching game process')
        return pid

    def control(self, command):
        if not command or command[0] not in ('profiler_ring', 'profiler_capture', 'profiler_ring_gpu', 'profiler_detail', 'profiler_hle', 'profiler_guest', 'profiler_tags', 'profiler_address', 'renderer_flush_image', 'renderer_hint', 'frontlock_status', 'lane_status', 'renderer_publication', 'renderer_invalidate', 'renderer_ordered_inputs', 'profiler_gpu', 'profiler_coverage', 'debug_status', 'renderer_status', 'renderer_resources', 'renderer_targets', 'renderer_schedule', 'renderer_flush', 'input_pulse', 'autoload_status', 'guest_modules', 'threads', 'help'):
            raise ValueError('Unsupported profiler command')
        if not all(re.fullmatch(r'[a-z0-9_]+', x) for x in command):
            raise ValueError('Invalid command argument')
        pid = self.pid()
        text = self.adb('shell', 'dumpsys', 'activity', 'service',
                        self.args.package+'/'+self.args.service, *command)
        service_pid = re.search(r'^SERVICE .*\bpid=(\d+)\b', text, re.M)
        if not service_pid or service_pid[1] != pid or self.pid() != pid:
            raise ValueError('Debug service is not owned by the selected game process')
        state = fields(text)
        if state.get('process_id', pid) != pid:
            raise ValueError('Runtime producer identity mismatch')
        if state.get('error'):
            raise ValueError(state['error'])
        return dict(state, process_id=pid)

    def collect(self, mode, artifact_id, output):
        prefix, command = ('dump', 'profiler_ring') if mode == 'ring' else ('capture', 'profiler_capture')
        state = self.control([command, 'status'])
        if state.get(prefix+'_id') != artifact_id or state.get(prefix+'_state') != 'ready':
            raise ValueError('Exact artifact is not ready')
        if mode == 'file' and (state.get('capture_active') != '0' or state.get('capture_mode') != 'file'):
            raise ValueError('File capture must be finalized')
        remote = state[prefix+'_path']
        if not remote.startswith('/data/user/0/'+self.args.package+'/files/') or not re.fullmatch(r'[A-Za-z0-9_/.-]+\.prof', remote) or '..' in remote:
            raise ValueError('Unsupported private capture path')
        metadata = json.loads(self.adb('exec-out', 'run-as', self.args.package, 'cat', remote+'.json'))
        if str(metadata.get('process_id')) != state['process_id']:
            raise ValueError('Capture sidecar PID mismatch')
        if mode == 'file' and (str(metadata.get('capture_id')) != artifact_id or metadata.get('capture_complete') is not True):
            raise ValueError('Capture sidecar is incomplete or has a different ID')
        size = int(self.adb('shell', 'run-as', self.args.package, 'stat', '-c', '%s', remote).strip())
        if not 0 < size <= 1024*1024*1024:
            raise ValueError('Capture size outside bounds')
        checksum = self.adb('shell', 'run-as', self.args.package, 'sha256sum', remote).split()[0]
        output = Path(output).resolve(); output.mkdir(parents=True, exist_ok=True)
        local = output/Path(remote).name
        if local.exists() or Path(str(local)+'.json').exists():
            raise ValueError('Refusing to overwrite capture')
        partial = Path(str(local)+'.partial')
        with partial.open('xb') as target:
            subprocess.run([self.args.adb, '-s', self.args.serial, 'exec-out', 'run-as', self.args.package,
                            'cat', remote], stdout=target, check=True, timeout=120)
        with partial.open('rb') as source:
            actual = hashlib.file_digest(source, 'sha256').hexdigest()
        if partial.stat().st_size != size or actual != checksum or self.pid() != state['process_id']:
            raise ValueError('Capture transfer or process identity changed; partial preserved')
        after = self.control([command, 'status'])
        if any(after.get(k) != state.get(k) for k in (prefix+'_id', prefix+'_path', prefix+'_state')):
            raise ValueError('Artifact changed during collection; partial preserved')
        partial.rename(local)
        Path(str(local)+'.json').write_text(json.dumps(metadata, indent=2)+'\n')
        provenance = dict(serial=self.args.serial, package=self.args.package,
                          process_suffix=self.args.process_suffix, service=self.args.service,
                          process_id=state['process_id'], artifact_id=artifact_id, mode=mode,
                          sha256=actual, source_path=remote, bytes=size, status=state)
        Path(str(local)+'.identity.json').write_text(json.dumps(provenance, indent=2)+'\n')
        return dict(path=str(local), **provenance)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default=os.getenv('ADB', 'adb'))
    parser.add_argument('--serial', required=True)
    parser.add_argument('--package', required=True)
    parser.add_argument('--service', default='org.macnative.android.PortDebugService')
    parser.add_argument('--process-suffix', default=':game')
    parser.add_argument('--expected-pid')
    subs = parser.add_subparsers(dest='action', required=True)
    control = subs.add_parser('control');control.add_argument('command', nargs='+')
    collect = subs.add_parser('collect');collect.add_argument('--mode', choices=['ring', 'file'], required=True)
    collect.add_argument('--id', required=True);collect.add_argument('--output', required=True)
    args = parser.parse_args(); target = Target(args)
    if args.action == 'control':
        for key, value in target.control(args.command).items(): print(f'{key}={value}')
    else:
        print(json.dumps(target.collect(args.mode, args.id, args.output), indent=2))


if __name__ == '__main__':
    main()
