#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Capture one bounded input segment; collect and analyze its exact ID in Litep."""
import argparse
import json
import time
from pathlib import Path
from debugbus import request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--direction', choices=('forward', 'backward', 'left', 'right', 'look_left', 'look_right'), default='forward')
    parser.add_argument('--duration-ms', type=int, default=1200)
    parser.add_argument('--seconds', type=int, default=4)
    parser.add_argument('--delay', type=float, default=.5)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not (1 <= args.duration_ms <= 3000 and 2 <= args.seconds <= 30 and
            0 <= args.delay <= args.seconds - args.duration_ms/1000 - .3):
        parser.error('Invalid bounded route timing')
    # Reserve the manifest before touching the running game.
    with args.output.open('x') as manifest:
        evidence = {'pid': args.pid, 'direction': args.direction,
                    'duration_ms': args.duration_ms, 'seconds': args.seconds, 'delay': args.delay,
                    'route_identity': 'Input request and HID consumption; validate movement duration with Input.Pulse markers and pose separately'}
        path = f'/tmp/mng-botw-{args.pid}.sock'
        capture_id = generation = None

        def control(command):
            status = request(path, 'debug_status')
            if f'process_id={args.pid}' not in status.splitlines():
                raise RuntimeError('Process identity changed or unavailable')
            response = request(path, command)
            fields = dict(line.split('=', 1) for line in response.splitlines() if '=' in line)
            if fields.get('error'):
                raise RuntimeError(response)
            return fields

        try:
            before = control('profiler_capture status')
            if before.get('capture_active') == '1' or control('input_pulse status').get('active') == '1':
                raise RuntimeError('Another capture or input pulse is active')
            evidence['capture_start'] = control(f'profiler_capture file 256 {args.seconds}')
            capture_id = evidence['capture_start']['capture_id']
            time.sleep(args.delay)
            evidence['input_start'] = control(f'input_pulse {args.direction} {args.duration_ms}')
            generation = evidence['input_start']['generation']
            deadline = time.monotonic() + args.seconds + 20
            while time.monotonic() < deadline:
                time.sleep(.1)
                state = control('profiler_capture status')
                if state.get('capture_id') != capture_id:
                    raise RuntimeError('Capture identity changed')
                if state.get('capture_active') == '0':
                    evidence['capture_end'] = state
                    if state.get('capture_state') != 'ready':
                        raise RuntimeError('Capture did not finish cleanly')
                    break
            else:
                raise TimeoutError('Capture completion timeout')
            evidence['input_end'] = control('input_pulse status')
            if evidence['input_end'].get('applied_generation') != generation or evidence['input_end'].get('active') != '0':
                raise RuntimeError('Guest HID did not observe the input pulse')
            evidence['complete'] = True
        except BaseException as error:
            evidence['error'] = str(error)
            raise
        finally:
            # Do not stop a later request belonging to another controller.
            try:
                state = control('input_pulse status')
                if generation and state.get('generation') == generation and state.get('active') == '1':
                    control('input_pulse stop')
                state = control('profiler_capture status')
                if capture_id and state.get('capture_id') == capture_id and state.get('capture_active') == '1':
                    control('profiler_capture stop')
            except Exception as error:
                evidence['cleanup_error'] = str(error)
            json.dump(evidence, manifest, indent=2)
    print(json.dumps({'pid': args.pid, 'capture_id': capture_id, 'manifest': str(args.output)}))


if __name__ == '__main__':
    main()
