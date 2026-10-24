#!/usr/bin/env python3
"""Execute synthetic original/replaced instructions; no game data or CPU emulator in the port."""
import ast
import random
import re
import struct
import unittest
from itertools import combinations
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *

SOURCE = Path(__file__).resolve().parents[1] / 'third_party/d2r/patch_nso.py'
BASE, STACK, TSD = 0x100000, 0x300000, 0x500000
SP = STACK + 0x10000
GP = [UC_ARM64_REG_X0 + i for i in range(29)] + [UC_ARM64_REG_X29, UC_ARM64_REG_X30]

def emit(word):
    tree=ast.parse(SOURCE.read_text())
    functions=[n for n in tree.body if isinstance(n,ast.FunctionDef)]
    spop=next(n for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='SPOP' for t in n.targets))
    md=Cs(CS_ARCH_ARM64,CS_MODE_ARM);md.detail=True
    n=dict(struct=struct,re=re,combinations=combinations,md=md,patch=bytearray(),fix=[],stats={},
           TARGET='android',NOREDZONE=True,PATCH_OFF=BASE+0x1000,KEY_PAGE_OFF=BASE+0x6000,
           KEY_INDEX={'K18':0},NOP=0xD503201F)
    exec(compile(ast.Module(body=[spop]+functions,type_ignores=[]),str(SOURCE),'exec'),n)
    ins=n['dis'](word,BASE+0x100)
    n['site_plain'](ins)
    return bytes(n['patch'])

def execute(word,seed,patched,signal):
    rng=random.Random(seed);u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    u.mem_map(BASE,0x10000);u.mem_map(STACK,0x20000);u.mem_map(TSD,0x10000)
    arena=rng.randbytes(0x20000);u.mem_write(STACK,arena)
    regs=[rng.getrandbits(64) for _ in range(31)]
    for r,v in zip(GP,regs):u.reg_write(r,v)
    u.reg_write(UC_ARM64_REG_SP,SP);u.reg_write(UC_ARM64_REG_NZCV,0xa0000000)
    u.reg_write(UC_ARM64_REG_TPIDR_EL0,TSD+0x8008)
    u.mem_write(TSD+0x8000,struct.pack('<Q',TSD))
    u.mem_write(TSD+0x100,struct.pack('<Q',regs[18]))
    u.mem_write(BASE+0x6000,struct.pack('<Q',0x100))
    u.mem_write(BASE+0x100,struct.pack('<I',word))
    if patched:
        u.mem_write(BASE+0x100,struct.pack('<I',0x140003c0)) # B +0xf00
        u.mem_write(BASE+0x1000,emit(word));u.reg_write(GP[18],0x12345678)
    def hook(uc,pc,size,unused):
        if pc==BASE+0x104:uc.emu_stop();return
        if signal:uc.mem_write(uc.reg_read(UC_ARM64_REG_SP)-128,bytes(128))
    u.hook_add(UC_HOOK_CODE,hook);u.emu_start(BASE+0x100,BASE+0x104,count=100)
    result=[u.reg_read(r) for r in GP]
    if patched:
        assert result[18]==0x12345678, 'Host x18 was clobbered'
        result[18]=struct.unpack('<Q',u.mem_read(TSD+0x100,8))[0]
    # Only the live stack at the final SP is observable (post-index loads release their slots).
    new_sp=u.reg_read(UC_ARM64_REG_SP);start=new_sp
    return result,new_sp,u.reg_read(UC_ARM64_REG_NZCV),bytes(u.mem_read(start,SP+0x100-start))

class AndroidRewrite(unittest.TestCase):
    def test_arithmetic_stack_reads_writes_and_writeback(self):
        words=[0x91001e52,0xf90013f2,0xf94013f2,0x910003f2,
               0xa9800000|(((-16//8)&127)<<15)|(19<<10)|(31<<5)|18,
               0xa8c00000|((16//8)<<15)|(19<<10)|(31<<5)|18]
        for word in words:
            for seed in range(32):
                with self.subTest(word=hex(word),seed=seed):
                    self.assertEqual(execute(word,seed,False,False),execute(word,seed,True,False))
                    # A signal may destroy any bytes below the current SP.
                    self.assertEqual(execute(word,seed,False,True),execute(word,seed,True,True))

if __name__=='__main__':unittest.main(verbosity=2)
