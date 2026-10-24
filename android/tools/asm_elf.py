#!/usr/bin/env python3
"""Translate the shared Mach-O gate syntax to ELF and its TLS loads to Bionic.

Syntax conversion adapted from D2R Mobile 758f5d4 (MIT, Copyright 2026 jhaizhou).
The TLS transformation is explicit; no guest instruction or address is involved.
"""
import argparse
import re
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('--bionic-tls', action='store_true')
args = parser.parse_args()
lines = []
for line in args.source.read_text().splitlines():
    code, sep, comment = line.partition('//')
    code = re.sub(r'(\w+)@PAGEOFF\b', r':lo12:\1', code)
    code = re.sub(r'(\w+)@PAGE\b', r'\1', code)
    code = re.sub(r'(?<![\w.$])_(\w+)', r'\1', code)
    if args.bionic_tls:
        code = re.sub(r'mrs\s+(x\d+),\s*tpidrro_el0\b',
                      lambda m: f'mrs {m[1]}, tpidr_el0\n    ldur {m[1]}, [{m[1]}, #-8]', code)
    lines.append(code + sep + comment)
lines.append('.section .note.GNU-stack,"",@progbits')
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text('\n'.join(lines) + '\n')
