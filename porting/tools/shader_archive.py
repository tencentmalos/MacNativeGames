#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Pack immutable precompiled .xgs/.xgv assets; no shader translation."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct


def pack(source, output):
    paths=sorted(p for p in source.iterdir() if re.fullmatch(r'[0-9a-f]{16}\.xg[sv]',p.name))
    if not paths or len(paths)>200000:
        raise ValueError('Invalid shader count')
    partial=output.with_suffix(output.suffix+'.partial')
    if output.exists() or Path(str(output)+'.json').exists():
        raise FileExistsError(output)
    entries=[];manifest=[]
    with partial.open('xb') as target:
        target.seek(32+40*len(paths))
        for i,path in enumerate(paths):
            data=path.read_bytes()
            if not 16<=len(data)<=16*1024*1024:
                raise ValueError('Invalid shader extent: '+path.name)
            offset=target.tell();target.write(data)
            entries.append(struct.pack('<24sQQ',path.name.encode(),offset,len(data)))
            manifest.append({'name':path.name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
            if (i+1)%10000==0:print(f'Packed {i+1}/{len(paths)}',flush=True)
        size=target.tell();target.seek(0)
        target.write(struct.pack('<8sIIQQ',b'MNGSHD1\0',1,len(paths),size,32))
        target.write(b''.join(entries));target.flush();os.fsync(target.fileno())
    # Atomic, no-overwrite publication. Keep failed prefixes for diagnosis.
    os.link(partial,output);partial.unlink()
    with output.open('rb') as src:sha=hashlib.file_digest(src,'sha256').hexdigest()
    with Path(str(output)+'.json').open('x') as dst:
        json.dump({'format':'mng-shader-archive-v1','source':str(source.resolve()),'sha256':sha,'bytes':size,'entries':manifest},dst,indent=2)
    return {'path':str(output),'sha256':sha,'bytes':size,'entries':len(entries)}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('output',type=Path)
    a=p.parse_args();print(json.dumps(pack(a.source,a.output)))
