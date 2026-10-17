#ifndef SPU_PIC_MATCH_H
#define SPU_PIC_MATCH_H

#include <stdint.h>
#include <string.h>

static inline int spu_pic_entry_matches(const uint8_t* ls, uint32_t ls_size, uint32_t pc,
                                        const uint8_t* code, uint32_t addr, uint32_t lo, uint32_t hi,
                                        const uint32_t* ranges, uint32_t nranges)
{
    if (pc < addr - lo) return 0;
    if (!nranges) {
        const uint32_t base = pc - (addr - lo), span = hi - lo;
        if (base + span > ls_size) return 0;
        return memcmp(ls + base, code + lo, span) == 0;
    }
    for (uint32_t i = 0; i < nranges; i++) {
        const uint32_t a = ranges[2 * i], b = ranges[2 * i + 1];
        if (a < lo || b > hi || b <= a || (a < addr && pc < addr - a)) return 0;
        const uint32_t at = pc + a - addr;
        if (at + (b - a) > ls_size) return 0;
        if (memcmp(ls + at, code + a, b - a) != 0) return 0;
    }
    return 1;
}

#endif
