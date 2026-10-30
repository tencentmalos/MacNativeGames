#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded local native-port debugbus client; usable as a Litep command target."""
import argparse
import socket

def request(path, command):
    data=command.encode()
    if len(data)>256 or b'\n' in data or b'\0' in data:
        raise ValueError('invalid debugbus command')
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(5)
        client.connect(path)
        client.sendall(data+b'\n')
        chunks=[];total=0
        while True:
            part=client.recv(65536)
            if not part:break
            total+=len(part)
            if total>1024*1024:raise ValueError('debugbus response limit')
            chunks.append(part)
    return b''.join(chunks).decode()

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--socket',required=True)
    parser.add_argument('--pid',type=int,required=True)
    parser.add_argument('command',nargs='+')
    args=parser.parse_args()
    status=request(args.socket,'debug_status')
    if f'process_id={args.pid}' not in status.splitlines():
        raise RuntimeError('debugbus process identity mismatch or busy; retry')
    response=request(args.socket,' '.join(args.command))
    if response.startswith('schema=foundation.profiler-ring.v1\n') and not any(
            line.startswith('process_id=') for line in response.splitlines()):
        # The shared ring schema has no PID. Supply the controller identity
        # only after checking the same endpoint again after the command.
        after=request(args.socket,'debug_status')
        if f'process_id={args.pid}' not in after.splitlines():
            raise RuntimeError('debugbus process changed or busy; retry')
        response=response.rstrip('\n')+f'\nprocess_id={args.pid}\n'
    print(response,end='')
if __name__=='__main__':main()
