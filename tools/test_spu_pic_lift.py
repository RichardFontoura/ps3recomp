#!/usr/bin/env python3
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from wrap_spu_elf import wrap


def ri16(op9, i16, rt):
    return (op9 << 23) | ((i16 & 0xFFFF) << 7) | rt


def bi(ra):
    return (0x1A8 << 21) | (ra << 7)


NOP = 0x40200000


def code():
    words = [ri16(0x066, 4, 0), ri16(0x067, 7, 3), bi(0), NOP, bi(0), NOP, NOP, NOP,
             0x11111111, 0x22222222, 0x33333333, 0x44444444]
    return b''.join(struct.pack('>I', w) for w in words)


def lift(pic):
    with tempfile.TemporaryDirectory() as tmp:
        elf = os.path.join(tmp, 'frag.elf')
        with open(elf, 'wb') as f:
            f.write(wrap(code(), base=0, entry=0))
        cmd = [sys.executable, os.path.join(HERE, 'spu_lifter.py'), '--auto-functions', elf, '--base', '0',
               '--symbol-prefix', 'frag_', '-o', tmp, '--code-end', '0x20']
        if pic:
            cmd.append('--pic')
        subprocess.run(cmd, check=True, capture_output=True, text=True)
        with open(os.path.join(tmp, 'spu_recomp.c')) as f:
            return f.read()


def main():
    failures = 0

    def check(cond, what):
        nonlocal failures
        if not cond:
            failures += 1
            print(f'FAIL: {what}')

    pic = lift(True)
    fixed = lift(False)
    check('spu_link(SPU_PIC(ctx, 0x4))' in pic, 'brsl link is relative to pic_base')
    check('spu_drain_call(ctx, SPU_PIC(ctx, 0x4))' in pic, 'call return pc is relative to pic_base')
    check('spu_ls_read128(ctx, SPU_PIC(ctx, 0x20))' in pic, 'lqr address is relative to pic_base')
    check('frag_pic_functions[]' in pic and 'frag_pic_function_count' in pic, 'PIC function table emitted')
    check('spu_register_function' not in pic, 'PIC image stays out of the address registry')
    check('SPU_PIC' not in fixed, 'fixed-address lift unchanged')
    check('spu_link(0x4)' in fixed, 'fixed-address link constant unchanged')
    print(f'SPU PIC lift: {"ok" if not failures else f"{failures} failure(s)"}')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
