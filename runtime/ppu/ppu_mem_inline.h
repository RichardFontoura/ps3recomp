#ifndef PPU_MEM_INLINE_H
#define PPU_MEM_INLINE_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__clang__) && defined(__x86_64__)
#define PPU_MEM_SLOW_CC __attribute__((preserve_most))
#else
#define PPU_MEM_SLOW_CC
#endif

#define PPU_MEM_FAST_LO 0x10000u
#define PPU_MEM_COH_LINE_SHIFT 7u

extern uint8_t* vm_base;
extern uint32_t ppu_hle_inject_base;
extern uint32_t g_null_sweep_hi;
extern uint32_t g_ppu_mem_fast_hi;
extern const unsigned char* const g_spu_coh_bitmap_ro;
extern const int* const g_spu_coh_armed_ro;

void ppu_mem_fast_init(void);
void ppu_mem_fast_set_enabled(int on);
PPU_MEM_SLOW_CC uint8_t ppu_mem_slow_read8(uint64_t a);
PPU_MEM_SLOW_CC uint16_t ppu_mem_slow_read16(uint64_t a);
PPU_MEM_SLOW_CC uint32_t ppu_mem_slow_read32(uint64_t a);
PPU_MEM_SLOW_CC uint64_t ppu_mem_slow_read64(uint64_t a);
PPU_MEM_SLOW_CC void ppu_mem_slow_write8(uint64_t a, uint8_t v);
PPU_MEM_SLOW_CC void ppu_mem_slow_write16(uint64_t a, uint16_t v);
PPU_MEM_SLOW_CC void ppu_mem_slow_write32(uint64_t a, uint32_t v);
PPU_MEM_SLOW_CC void ppu_mem_slow_write64(uint64_t a, uint64_t v);

static inline int ppu_mem_fast_range(uint32_t ea, uint32_t n)
{
    return ea >= PPU_MEM_FAST_LO && (uint64_t)ea + n <= g_ppu_mem_fast_hi;
}

static inline int ppu_mem_fast_store(uint32_t ea, uint32_t n)
{
    if (!ppu_mem_fast_range(ea, n) || g_null_sweep_hi) return 0;
    if (!*g_spu_coh_armed_ro) return 1;
    const uint32_t line = ea >> PPU_MEM_COH_LINE_SHIFT;
    return !((g_spu_coh_bitmap_ro[line >> 3] >> (line & 7u)) & 1u);
}

static inline uint8_t ppu_fast_read8(uint64_t a)
{
    const uint32_t ea = (uint32_t)a;
    if (ppu_mem_fast_range(ea, 1)) return vm_base[ea];
    return ppu_mem_slow_read8(a);
}

static inline uint16_t ppu_fast_read16(uint64_t a)
{
    const uint32_t ea = (uint32_t)a;
    if (ppu_mem_fast_range(ea, 2)) {
        uint16_t v;
        memcpy(&v, vm_base + ea, 2);
        return __builtin_bswap16(v);
    }
    return ppu_mem_slow_read16(a);
}

static inline uint32_t ppu_fast_read32(uint64_t a)
{
    const uint32_t ea = (uint32_t)a;
    if (ppu_mem_fast_range(ea, 4) && ea != ppu_hle_inject_base + 0x2008u) {
        uint32_t v;
        memcpy(&v, vm_base + ea, 4);
        return __builtin_bswap32(v);
    }
    return ppu_mem_slow_read32(a);
}

static inline uint64_t ppu_fast_read64(uint64_t a)
{
    const uint32_t ea = (uint32_t)a;
    if (ppu_mem_fast_range(ea, 8)) {
        uint64_t v;
        memcpy(&v, vm_base + ea, 8);
        return __builtin_bswap64(v);
    }
    return ppu_mem_slow_read64(a);
}

static inline void ppu_fast_write8(uint64_t a, uint8_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (ppu_mem_fast_store(ea, 1)) { vm_base[ea] = v; return; }
    ppu_mem_slow_write8(a, v);
}

static inline void ppu_fast_write16(uint64_t a, uint16_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (ppu_mem_fast_store(ea, 2)) {
        const uint16_t b = __builtin_bswap16(v);
        memcpy(vm_base + ea, &b, 2);
        return;
    }
    ppu_mem_slow_write16(a, v);
}

static inline void ppu_fast_write32(uint64_t a, uint32_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (ppu_mem_fast_store(ea, 4)) {
        const uint32_t b = __builtin_bswap32(v);
        memcpy(vm_base + ea, &b, 4);
        return;
    }
    ppu_mem_slow_write32(a, v);
}

static inline void ppu_fast_write64(uint64_t a, uint64_t v)
{
    const uint32_t ea = (uint32_t)a;
    if (ppu_mem_fast_store(ea, 8)) {
        const uint64_t b = __builtin_bswap64(v);
        memcpy(vm_base + ea, &b, 8);
        return;
    }
    ppu_mem_slow_write64(a, v);
}

#if defined(PPU_MEM_INLINE) && PPU_MEM_INLINE
#define vm_read8 ppu_fast_read8
#define vm_read16 ppu_fast_read16
#define vm_read32 ppu_fast_read32
#define vm_read64 ppu_fast_read64
#define vm_write8 ppu_fast_write8
#define vm_write16 ppu_fast_write16
#define vm_write32 ppu_fast_write32
#define vm_write64 ppu_fast_write64
#endif

#ifdef __cplusplus
}
#endif

#endif
