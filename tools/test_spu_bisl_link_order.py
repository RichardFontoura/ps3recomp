#!/usr/bin/env python3
import os
import re
import struct
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

from spu_disasm import disassemble_spu  # noqa: E402
from spu_lifter import SPULifter  # noqa: E402


def w_rr(op11, rt, ra, rb=0, de=0):
    return (op11 << 21) | (rb << 14) | (ra << 7) | rt | de


def lift(words):
    data = b"".join(struct.pack(">I", w) for w in words)
    insns = disassemble_spu(data, 0x1000)
    lifter = SPULifter(prefix="t_")
    lifter.func_starts = {0x1000}
    func = lifter.lift_function(insns, 0x1000, 0x1000 + 4 * len(words))
    return "\n".join(func.body_lines)


def check(name, body, reg):
    m_target = re.search(r"_bt = ctx->gpr\[%d\]\._u32\[0\]" % reg, body)
    m_link = re.search(r"ctx->gpr\[%d\] = spu_link\(" % reg, body)
    ok = bool(m_target and m_link and m_target.start() < m_link.start()
              and "ctx->pc = _bt;" in body)
    print(f"{name}: {'PASS' if ok else 'FAIL'}")
    if not ok:
        print(body)
    return ok


def main():
    stop = 0
    results = [
        check("bisl $r0,$r0 reads the target before writing the link",
              lift([w_rr(0x1A9, 0, 0), stop]), 0),
        check("bisl $r5,$r5 reads the target before writing the link",
              lift([w_rr(0x1A9, 5, 5), stop]), 5),
        check("bisled $r7,$r7 reads the target before writing the link",
              lift([w_rr(0x1AB, 7, 7), stop]), 7),
    ]
    other = lift([w_rr(0x1A9, 0, 5), stop])
    ok = "_bt = ctx->gpr[5]._u32[0]" in other and "ctx->pc = _bt;" in other
    print(f"bisl $r0,$r5 still targets r5: {'PASS' if ok else 'FAIL'}")
    results.append(ok)
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
