#!/usr/bin/env python3
import os
import struct
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

from find_spu_functions import detect_functions  # noqa: E402
from wrap_spu_elf import wrap  # noqa: E402

BASE = 0x4000
BI_R0 = 0x35000000
LNOP = 0x00200000
STQD_R80_M20_R1 = 0x24FF80D0


def il(rt, value):
    return (0x081 << 23) | ((value & 0xFFFF) << 7) | rt


def module(pointer_rows):
    words = [
        il(3, 0), il(4, 1), LNOP, BI_R0,
        STQD_R80_M20_R1, il(3, 1), LNOP, BI_R0,
        il(3, 2), LNOP, LNOP, BI_R0,
    ]
    for row in pointer_rows:
        words += list(row) + [1, 0]
    return b"".join(struct.pack(">I", w) for w in words)


def starts(code):
    funcs, _ = detect_functions(wrap(code, base=BASE, entry=BASE), verbose=False)
    return {s for s, _ in funcs}


def check(name, ok):
    print(f"{name}: {'PASS' if ok else 'FAIL'}")
    return ok


def main():
    found = starts(module([(0x4010, 0x4020)]))
    rejected = starts(module([(0x4014, 0x4024)]))
    repeated = starts(module([(0x4010, 0x4010)]))
    results = [
        check("pair of pointers to function starts, one of them a leaf, seeds both",
              {0x4010, 0x4020} <= found),
        check("pair of pointers into function bodies seeds nothing",
              not ({0x4014, 0x4024} & rejected)),
        check("repeated pointer is not a table", 0x4010 not in repeated),
    ]
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
