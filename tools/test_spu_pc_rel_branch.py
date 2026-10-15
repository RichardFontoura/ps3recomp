#!/usr/bin/env python3
import os
import struct
import sys

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

from find_spu_functions import detect_functions  # noqa: E402
from spu_disasm import disassemble_spu  # noqa: E402
from spu_lifter import SPULifter, compute_pc_relative_branches  # noqa: E402
from wrap_spu_elf import wrap  # noqa: E402

BRSL_R2_NEXT = 0x33000082
AI_R2_R2_8 = 0x1C020102
BIHZD_R5_R2 = 0x25480105
BIHZE_R5_R2 = 0x25440105
BIHZ_R5_R2 = 0x25400105
AI_R5_R9_M1 = 0x1CFFC485
BI_R0 = 0x35000000


def lift(words, pic=False):
    data = b"".join(struct.pack(">I", w) for w in words)
    insns = disassemble_spu(data, 0x1000)
    lifter = SPULifter(prefix="t_")
    lifter.pic = pic
    lifter.func_starts = {0x1000}
    lifter.pc_rel_branch = compute_pc_relative_branches(insns)
    func = lifter.lift_function(insns, 0x1000, 0x1000 + 4 * len(words))
    return "\n".join(func.body_lines)


def starts(words):
    data = b"".join(struct.pack(">I", w) for w in words)
    funcs, _ = detect_functions(wrap(data, base=0x1000, entry=0x1000), verbose=False)
    return {s for s, _ in funcs}


def check(name, ok, body):
    print(f"{name}: {'PASS' if ok else 'FAIL'}")
    if not ok:
        print(body)
    return ok


def main():
    disable = lift([BRSL_R2_NEXT, AI_R2_R2_8, BIHZD_R5_R2, AI_R5_R9_M1, BI_R0])
    plain = lift([BRSL_R2_NEXT, AI_R2_R2_8, BIHZ_R5_R2, AI_R5_R9_M1, BI_R0])
    enable = lift([BRSL_R2_NEXT, AI_R2_R2_8, BIHZE_R5_R2, AI_R5_R9_M1, BI_R0])
    pic = lift([BRSL_R2_NEXT, AI_R2_R2_8, BIHZD_R5_R2, AI_R5_R9_M1, BI_R0], pic=True)
    guarded = "if (ctx->gpr[2]._u32[0] == 0x100Cu) goto loc_0000100C;"
    results = [
        check("bihzd to brsl+ai constant jumps locally, interrupts disabled",
              guarded in disable and "loc_0000100C:" in disable
              and disable.index("ctx->int_enable = 0;") < disable.index(guarded), disable),
        check("constant mismatch still dispatches through the register",
              "ctx->pc = ctx->gpr[2]._u32[0]; g_spu_trampoline_fn = spu_indirect_branch;"
              in disable, disable),
        check("bihz without interrupt bits jumps locally", guarded in plain, plain),
        check("bihze keeps the dispatcher so a pending interrupt can be taken",
              "goto loc_0000100C" not in enable, enable),
        check("position-independent lift keeps the dispatcher", "goto loc_0000100C" not in pic, pic),
        check("bihze target becomes a function entry",
              0x100C in starts([BRSL_R2_NEXT, AI_R2_R2_8, BIHZE_R5_R2, AI_R5_R9_M1, BI_R0]), enable),
        check("bihzd target stays inside its function",
              0x100C not in starts([BRSL_R2_NEXT, AI_R2_R2_8, BIHZD_R5_R2, AI_R5_R9_M1, BI_R0]), disable),
    ]
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
