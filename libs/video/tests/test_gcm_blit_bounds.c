#include "../cellGcmSys.c"
#include <assert.h>
#include <stdlib.h>
#include <sys/mman.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
ps3_guest_caller_fn g_ps3_guest_caller;

static int s_reserved_line = -1;
static int s_notified;

void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { return (int)a == s_reserved_line; }
void spu_coh_notify_write(uint32_t a) { if ((int)a == s_reserved_line) s_notified++; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

static void fill(u32 ea, u32 bytes, u8 value)
{
    memset(vm_base + ea, value, bytes);
}

static void test_row_bounds(void)
{
    assert(nv3089_row_ok(0x10000000u, 64));
    assert(nv3089_row_ok(0xCFFFF000u, 0x1000));
    assert(!nv3089_row_ok(0xCFFFF800u, 0x1000));
    assert(!nv3089_row_ok(0xD0000000u, 16));
    assert(!nv3089_row_ok(0x1FFFFFF0u, 0x20));
    assert(!nv3089_row_ok(0x10000000u, 0));
}

static void test_unscaled_copy(void)
{
    fill(0xC0000000u, 0x4000, 0xAB);
    fill(0xC0100000u, 0x4000, 0x00);
    s_reserved_line = 0xC0100080;
    s_notified = 0;
    assert(nv3089_copy_unscaled(0xC0000000u, 0xC0100000u, 0x1000, 0x1000, 0x400, 4));
    for (u32 r = 0; r < 4; r++) {
        assert(vm_base[0xC0100000u + r * 0x1000] == 0xAB);
        assert(vm_base[0xC0100000u + r * 0x1000 + 0x3FF] == 0xAB);
        assert(vm_base[0xC0100000u + r * 0x1000 + 0x400] == 0x00);
    }
    assert(s_notified == 1);
    s_reserved_line = -1;
}

static void test_rows_past_local_memory_are_dropped(void)
{
    fill(0xC0200000u, 0x4000, 0x5C);
    fill(0xCFFFE000u, 0x2000, 0x00);
    assert(nv3089_copy_unscaled(0xC0200000u, 0xCFFFE000u, 0x1000, 0x1000, 0x1000, 3));
    assert(vm_base[0xCFFFE000u] == 0x5C);
    assert(vm_base[0xCFFFF000u + 0xFFF] == 0x5C);
}

static void test_source_beyond_arena_falls_back(void)
{
    ppu_vm_size = 0xE0000000u;
    assert(!nv3089_copy_unscaled(0xFFEF0000u, 0xC0300000u, 0x1000, 0x1000, 0x1000, 2));
    ppu_vm_size = 0;
}

int main(void)
{
    void* arena = mmap(NULL, 0x100000000ull, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    assert(arena != MAP_FAILED);
    vm_base = (uint8_t*)arena;
    mprotect(vm_base + 0xD0000000u, 0x1000, PROT_NONE);
    test_row_bounds();
    test_unscaled_copy();
    test_rows_past_local_memory_are_dropped();
    test_source_beyond_arena_falls_back();
    printf("NV3089 blit bounds checks passed\n");
    return 0;
}
