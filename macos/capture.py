#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Request one on-demand MoltenVK frame from an exact live native-port PID."""
import argparse
import json
import os
from pathlib import Path
import plistlib
import stat
import subprocess
import time

from debugbus import request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--output', type=Path, required=True,
                        help='The new .gputrace path passed to run.py')
    args = parser.parse_args()
    if args.pid <= 0:
        parser.error('invalid PID')
    output = args.output.resolve()
    if output.suffix != '.gputrace' or output.exists():
        parser.error('capture output must be a new .gputrace path')
    socket = f'/tmp/mng-botw-{args.pid}.sock'
    identity = request(socket, 'debug_status')
    fields = dict(line.split('=', 1) for line in identity.splitlines() if '=' in line)
    if fields.get('process_id') != str(args.pid) or 'backend=vulkan' not in fields.get('title', '').split():
        parser.error('native Vulkan process identity mismatch or busy')
    opened = subprocess.run(['/usr/sbin/lsof', '-a', '-p', str(args.pid), '-Fn'],
                            text=True, capture_output=True, check=True, timeout=10).stdout
    pipes = {Path(line[1:]) for line in opened.splitlines()
             if line.startswith('n') and Path(line[1:]).name.startswith('MoltenVKCapturePipe-')}
    if len(pipes) != 1:
        parser.error('expected one capture pipe; launch with --gpu-capture first')
    pipe = pipes.pop()
    fd = os.open(pipe, os.O_WRONLY | os.O_NONBLOCK | os.O_NOFOLLOW)
    try:
        info = os.fstat(fd)
        if not stat.S_ISFIFO(info.st_mode) or info.st_uid != os.getuid():
            raise RuntimeError('invalid capture pipe type or owner')
        if os.write(fd, b'1') != 1:
            raise RuntimeError('incomplete capture request')
    finally:
        os.close(fd)
    result = {'pid': args.pid, 'pipe': str(pipe), 'requested_frames': 1,
              'output': str(output), 'status': 'requested', 'capture_success': 'unverified'}
    # The directory appears before capture has finished. Wait for the archive
    # publisher's metadata/index instead of importing a half-written capture.
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        if (output / 'index').is_file() and (output / 'metadata').is_file():
            try:
                metadata = plistlib.loads((output / 'metadata').read_bytes())
                result['captured_frames'] = metadata['DYCaptureEngine.captured_frames_count']
                result['status'] = 'archive_published'
                break
            except (OSError, ValueError, KeyError, plistlib.InvalidFileException):
                pass
        time.sleep(.25)
    print(json.dumps(result))
    if result['status'] != 'archive_published':
        raise SystemExit('Capture publication timed out; retain evidence and inspect the target log')


if __name__ == '__main__':
    main()
