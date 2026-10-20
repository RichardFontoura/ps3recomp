#include "ppu_mem_inline.h"
#include <stdio.h>
#include <stdlib.h>

#define PPU_MEM_FAST_CEILING 0xE0000000u

extern uint32_t ppu_vm_size;
uint8_t vm_read8(uint64_t addr);
uint16_t vm_read16(uint64_t addr);
uint32_t vm_read32(uint64_t addr);
uint64_t vm_read64(uint64_t addr);
void vm_write8(uint64_t addr, uint8_t val);
void vm_write16(uint64_t addr, uint16_t val);
void vm_write32(uint64_t addr, uint32_t val);
void vm_write64(uint64_t addr, uint64_t val);

uint32_t g_ppu_mem_fast_hi = 0;
static uint32_t s_ppu_mem_fast_configured;

static const char* const k_diagnostics[] = {
    "PPU_FORCE_READ_ADDR", "PPU_HOTMAP", "PPU_RVAL", "PPU_RVAL64", "PPU_RWATCH", "PPU_SPINBT",
    "PPU_WVAL", "PPU_WWATCH", "PPU_WW_GUARD", "PT", "YDKJ_RWATCH", "PS3_TIMELINE", "PS3_TIMELINE_AT",
};

void ppu_mem_fast_init(void)
{
    g_ppu_mem_fast_hi = 0;
    const char* e = getenv("PPU_INLINE_MEM");
    const char* why = (e && e[0] == '0') ? "PPU_INLINE_MEM=0" : NULL;
    for (unsigned i = 0; !why && i < sizeof k_diagnostics / sizeof k_diagnostics[0]; i++)
        if (getenv(k_diagnostics[i])) why = k_diagnostics[i];
    if (!why) {
        uint32_t hi = PPU_MEM_FAST_CEILING;
        if (ppu_vm_size && ppu_vm_size < hi) hi = ppu_vm_size;
        g_ppu_mem_fast_hi = hi;
    }
    s_ppu_mem_fast_configured = g_ppu_mem_fast_hi;
    fprintf(stderr, "[PPU] inline guest memory access: %s%s%s\n", why ? "off (" : "on",
            why ? why : "", why ? ")" : "");
}

PPU_MEM_SLOW_CC uint8_t ppu_mem_slow_read8(uint64_t a) { return vm_read8(a); }
PPU_MEM_SLOW_CC uint16_t ppu_mem_slow_read16(uint64_t a) { return vm_read16(a); }
PPU_MEM_SLOW_CC uint32_t ppu_mem_slow_read32(uint64_t a) { return vm_read32(a); }
PPU_MEM_SLOW_CC uint64_t ppu_mem_slow_read64(uint64_t a) { return vm_read64(a); }
PPU_MEM_SLOW_CC void ppu_mem_slow_write8(uint64_t a, uint8_t v) { vm_write8(a, v); }
PPU_MEM_SLOW_CC void ppu_mem_slow_write16(uint64_t a, uint16_t v) { vm_write16(a, v); }
PPU_MEM_SLOW_CC void ppu_mem_slow_write32(uint64_t a, uint32_t v) { vm_write32(a, v); }
PPU_MEM_SLOW_CC void ppu_mem_slow_write64(uint64_t a, uint64_t v) { vm_write64(a, v); }

void ppu_mem_fast_set_enabled(int on)
{
    __atomic_store_n(&g_ppu_mem_fast_hi, on ? s_ppu_mem_fast_configured : 0u, __ATOMIC_RELAXED);
}
