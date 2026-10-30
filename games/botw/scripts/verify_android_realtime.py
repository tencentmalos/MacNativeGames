#!/usr/bin/env python3
"""Compare the actual Android patch output with the Mac BOTW160 recipe, then
execute its VFR/fade/UI/physics/controller checks on the same private input.
No module bytes are saved in the repository. Requires Unicorn.
"""
import argparse
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import sys

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('original',type=Path)
parser.add_argument('patched',type=Path)
a=parser.parse_args()
original=a.original.read_bytes();patched=a.patched.read_bytes()
assert hashlib.sha256(original).hexdigest()=='cf5d57a21cd85a627de7c9c500957d971b81b6aebbc3360ed78edbe350b41884'
assert len(original)==len(patched)==48355632
root=Path(__file__).resolve().parents[3]
source=(root/'games/totk/runtime/game_patches_botw.c').read_text()
page=(len(original)+0x3fff)&~0x3fff

def adrp(rd,site):
    imm=((page>>12)-(site>>12))&0x1fffff
    return 0x90000000|((imm&3)<<29)|((imm>>2)<<5)|rd

section=source.split('static void patch_realtime(Module *m) {',1)[1]
put=section.split('const struct { uint32_t off, word; } put[] = {',1)[1].split('\n    };',1)[0]
expected={}
for off,value in re.findall(r'\{(0x[0-9a-f]+)u, ([^}]+)\}',put):
    off=int(off,16)
    if value.startswith('adrp_reg'):
        reg,site=re.search(r'adrp_reg\((\d+), .*?\+ (0x[0-9a-f]+), page\)',value).groups()
        assert int(site,16)==off;word=adrp(int(reg),off)
    elif value=='nop':word=0xd503201f
    else:word=int(value.rstrip('u'),16)
    expected[off]=word
for name,base,count,index,rd in [('pad_loop',0x126e490,38,7,16),('menu_cd',0x1214474,9,2,13)]:
    for suffix,blob in [('shipped',original),('ticked',patched)]:
        array=re.search(r'static const uint32_t '+name+'_'+suffix+r'\[[^]]+\] = \{(.*?)\};',source,re.S)[1]
        array=re.sub(r'/\*.*?\*/','',array,flags=re.S)
        words=[int(x.strip().rstrip('u'),0) for x in array.split(',') if x.strip()]
        assert len(words)==count
        for i,w in enumerate(words):
            off=base+4*i
            if suffix=='ticked' and i==index:w=adrp(rd,off)
            assert struct.unpack_from('<I',blob,off)[0]==w,(name,suffix,hex(off))
            if suffix=='ticked':expected[off]=w
assert len(expected)==60
for off,w in expected.items():assert struct.unpack_from('<I',patched,off)[0]==w,hex(off)
print('PASS: all 60 Android emitted instruction words match the Mac recipe at the Android timing-page offset',flush=True)
for script in ['verify_realtime_patch.py','verify_pad_repeat_patch.py']:
    subprocess.run([sys.executable,str(Path(__file__).with_name(script)),str(a.original)],check=True)

# Execute Android's emitted 1.6.0 UI block and physics argument/solver blocks.
# The older Mac verifier covers these only for 1.9.0; do not claim that as 1.6.0 evidence.
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
from unicorn import arm64_const as A

def machine(blob):
    mu=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    mu.mem_map(0,page+16384);mu.mem_write(0,blob)
    mu.mem_map(0x10000000,0x10000)
    mu.reg_write(A.UC_ARM64_REG_SP,0x1000f000)
    return mu

def sf(mu,r,v):mu.reg_write(r,struct.unpack('<I',struct.pack('<f',v))[0])
def rf(mu,r):return struct.unpack('<f',struct.pack('<I',mu.reg_read(r)&0xffffffff))[0]
for hz in [15,20,30,60,120]:
    seconds=1/hz;ratio=seconds*30
    for enabled,blob in [(False,original),(True,patched)]:
        mu=machine(blob)
        mu.mem_write(page,struct.pack('<ff',ratio,seconds))
        mu.mem_write(0x10000000,struct.pack('<I',2))
        mu.reg_write(A.UC_ARM64_REG_X8,0x10000000)
        mu.reg_write(A.UC_ARM64_REG_X21,0x10001000)
        mu.emu_start(0x18522ec,0x1852308,count=16)
        ui=struct.unpack('<f',mu.mem_read(0x10001b20,4))[0]
        assert abs(ui-(ratio if enabled else 1))<1e-6,(hz,enabled,ui)
        mu.mem_write(0x10000000,struct.pack('<f',ratio))
        mu.reg_write(A.UC_ARM64_REG_X8,0x10000000)
        mu.emu_start(0x1618480,0x1618484,count=1)
        sf(mu,A.UC_ARM64_REG_S8,seconds)
        # Verified argument move and branch into the actual solver function.
        mu.reg_write(A.UC_ARM64_REG_X19,0x10002000)
        mu.mem_write(0x10002030,struct.pack('<Q',0x10003000))
        mu.mem_write(0x10003000,struct.pack('<Q',0x10004000))
        mu.mem_write(0x10004008,struct.pack('<Q',0x10005000))
        sf(mu,A.UC_ARM64_REG_S2,1)
        mu.emu_start(0x16184bc,0x1c9c998,count=100)
        count=struct.unpack('<I',mu.mem_read(0x100053dc,4))[0]
        assert count==(6 if enabled else int(6*ratio+.5)),(hz,enabled,count)
        assert abs(rf(mu,A.UC_ARM64_REG_S0)-seconds)<1e-7
print('PASS: actual Android BOTW160 UI and six physics substeps at 15/20/30/60/120 Hz; dt preserved; stock negative controls')
