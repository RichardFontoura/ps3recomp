#include "../ppu_mem_inline.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ARENA = 0x00400000 };

uint8_t* vm_base;
uint32_t ppu_vm_size = ARENA;
uint32_t ppu_hle_inject_base = 0x00300000u;
uint32_t g_null_sweep_hi = 0;

static unsigned char g_bitmap[(0x100000000ull >> PPU_MEM_COH_LINE_SHIFT) / 8u];
static int g_armed;
const unsigned char* const g_spu_coh_bitmap_ro = g_bitmap;
const int* const g_spu_coh_armed_ro = &g_armed;

static unsigned g_slow;
static int g_pass, g_fail;
static uint64_t g_rng = 0x853C49E6748FEA9Bull;

uint8_t vm_read8(uint64_t a) { g_slow++; return vm_base[(uint32_t)a % ARENA]; }
uint16_t vm_read16(uint64_t a) { g_slow++; (void)a; return 0xBEEF; }
uint32_t vm_read32(uint64_t a) { g_slow++; (void)a; return 0xDEADBEEFu; }
uint64_t vm_read64(uint64_t a) { g_slow++; (void)a; return 0x0123456789ABCDEFull; }
void vm_write8(uint64_t a, uint8_t v) { g_slow++; (void)a; (void)v; }
void vm_write16(uint64_t a, uint16_t v) { g_slow++; (void)a; (void)v; }
void vm_write32(uint64_t a, uint32_t v) { g_slow++; (void)a; (void)v; }
void vm_write64(uint64_t a, uint64_t v) { g_slow++; (void)a; (void)v; }

#include "../ppu_mem_fast.c"

static uint64_t rnd64(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}

static void check(int ok, const char* what)
{
    if (ok) { g_pass++; return; }
    g_fail++;
    printf("  FAIL: %s\n", what);
}

static uint64_t be_load(uint32_t ea, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | vm_base[ea + (uint32_t)i];
    return v;
}

static void test_fast_reads(void)
{
    int ok = 1;
    const unsigned before = g_slow;
    for (int i = 0; i < 200000; i++) {
        const uint32_t ea = PPU_MEM_FAST_LO + (uint32_t)(rnd64() % (ARENA - PPU_MEM_FAST_LO - 8u));
        if (ea == ppu_hle_inject_base + 0x2008u) continue;
        ok &= ppu_fast_read8(ea) == (uint8_t)be_load(ea, 1);
        ok &= ppu_fast_read16(ea) == (uint16_t)be_load(ea, 2);
        ok &= ppu_fast_read32(ea) == (uint32_t)be_load(ea, 4);
        ok &= ppu_fast_read64(ea) == be_load(ea, 8);
    }
    check(ok, "in-range reads return the big-endian guest value");
    check(g_slow == before, "in-range reads never take the slow path");
}

static void test_fast_writes(void)
{
    int ok = 1;
    const unsigned before = g_slow;
    for (int i = 0; i < 200000; i++) {
        const uint32_t ea = PPU_MEM_FAST_LO + (uint32_t)(rnd64() % (ARENA - PPU_MEM_FAST_LO - 8u));
        const uint64_t v = rnd64();
        ppu_fast_write8(ea, (uint8_t)v); ok &= be_load(ea, 1) == (uint8_t)v;
        ppu_fast_write16(ea, (uint16_t)v); ok &= be_load(ea, 2) == (uint16_t)v;
        ppu_fast_write32(ea, (uint32_t)v); ok &= be_load(ea, 4) == (uint32_t)v;
        ppu_fast_write64(ea, v); ok &= be_load(ea, 8) == v;
    }
    check(ok, "in-range writes store the big-endian guest value");
    check(g_slow == before, "in-range writes on unreserved lines never take the slow path");
}

static void test_slow_paths(void)
{
    unsigned before = g_slow;
    (void)ppu_fast_read32(0x0000FFF0u);
    check(g_slow == before + 1, "a read below 0x10000 takes the slow path");
    before = g_slow;
    (void)ppu_fast_read64(ARENA - 4u);
    check(g_slow == before + 1, "a read crossing the end of the guest space takes the slow path");
    before = g_slow;
    (void)ppu_fast_read32(ppu_hle_inject_base + 0x2008u);
    check(g_slow == before + 1, "a 32-bit read of the GCM ref register takes the slow path");
    before = g_slow;
    (void)ppu_fast_read8(ppu_hle_inject_base + 0x2008u);
    check(g_slow == before, "an 8-bit read of the same address stays fast, as vm_read8 has no hook there");
    before = g_slow;
    ppu_fast_write32(0x00000100u, 1);
    check(g_slow == before + 1, "a store below 0x10000 takes the slow path");
    g_armed = 1;
    const uint32_t reserved = 0x00200080u, other = 0x00200100u;
    g_bitmap[(reserved >> PPU_MEM_COH_LINE_SHIFT) >> 3] |= (unsigned char)(1u << ((reserved >> PPU_MEM_COH_LINE_SHIFT) & 7u));
    before = g_slow;
    ppu_fast_write64(reserved + 8u, 7);
    check(g_slow == before + 1, "a store to a reserved line takes the slow path");
    before = g_slow;
    ppu_fast_write64(other, 7);
    check(g_slow == before && be_load(other, 8) == 7, "a store to an unreserved line stays fast once armed");
    g_null_sweep_hi = 1;
    before = g_slow;
    ppu_fast_write32(other, 9);
    check(g_slow == before + 1, "stores take the slow path while a null sweep is being tracked");
    g_null_sweep_hi = 0;
    g_armed = 0;
}

static void test_switches(void)
{
    setenv("PPU_INLINE_MEM", "0", 1);
    ppu_mem_fast_init();
    unsigned before = g_slow;
    (void)ppu_fast_read32(0x00100000u);
    ppu_fast_write32(0x00100000u, 1);
    check(g_slow == before + 2 && g_ppu_mem_fast_hi == 0, "PPU_INLINE_MEM=0 sends everything to the slow path");
    unsetenv("PPU_INLINE_MEM");
    setenv("PPU_RWATCH", "100000", 1);
    ppu_mem_fast_init();
    before = g_slow;
    (void)ppu_fast_read32(0x00100000u);
    check(g_slow == before + 1, "an accessor diagnostic turns the fast path off");
    unsetenv("PPU_RWATCH");
    ppu_vm_size = 0;
    ppu_mem_fast_init();
    check(g_ppu_mem_fast_hi == PPU_MEM_FAST_CEILING, "an unchecked guest space is fast up to the raw SPU window");
    ppu_vm_size = ARENA;
    ppu_mem_fast_init();
}

int main(void)
{
    vm_base = (uint8_t*)malloc(ARENA + 16u);
    if (!vm_base) return 2;
    for (uint32_t i = 0; i < ARENA; i++) vm_base[i] = (uint8_t)rnd64();
    ppu_mem_fast_init();
    check(g_ppu_mem_fast_hi == ARENA, "the fast range ends at ppu_vm_size");
    test_fast_reads();
    test_fast_writes();
    test_slow_paths();
    test_switches();
    printf("PPU inline memory access: %d passed, %d failed\n", g_pass, g_fail);
    free(vm_base);
    return g_fail != 0;
}
