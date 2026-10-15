#include "../cellGcmSys.c"
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>

uint8_t* vm_base;
uint32_t ppu_vm_size;
int g_resv_store_active;
uint32_t g_ww_lo, g_ww_hi;
ps3_guest_caller_fn g_ps3_guest_caller;

enum {
    CTX_EA     = 0x1000,
    RING_BEGIN = 0x100000,
    SEG_END    = 0x13FFFC,
    VM_BYTES   = 4u << 20,
};

void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
int spu_coh_is_reserved(uint32_t a) { (void)a; return 0; }
void spu_coh_notify_write(uint32_t a) { (void)a; }
void spu_lockline_lock(void) {}
void spu_lockline_unlock(void) {}

static void reset_ring(u32 cur, u32 get)
{
    memset(vm_base + RING_BEGIN, 0, SEG_END + 4 - RING_BEGIN);
    vm_write32(CTX_EA + 0, RING_BEGIN);
    vm_write32(CTX_EA + 4, SEG_END);
    vm_write32(CTX_EA + 8, cur);
    s_fifo_getoff = get;
}

static void flip_lands_at_current_without_recycling(void)
{
    const u32 cur = 0x13F000;
    reset_ring(cur, 0x125000);
    assert(gcm_flip_into_fifo(CTX_EA, 0) == 1);
    assert(vm_read32(cur) == (GCM_FLIP_MARKER | 0u));
    assert(vm_read32(cur + 4) == 0);
    assert(vm_read32(CTX_EA + 8) == cur + 4);
}

static void flip_near_segment_end_stays_in_order(void)
{
    const u32 cur = SEG_END - 0x800;
    reset_ring(cur, 0x125000);
    assert(gcm_flip_into_fifo(CTX_EA, 1) == 1);
    assert(vm_read32(cur) == (GCM_FLIP_MARKER | 1u));
    assert(vm_read32(CTX_EA + 8) == cur + 4);
}

static void overran_title_still_recycles(void)
{
    const u32 cur = 0x13E000;
    s_title_overran = 1;
    reset_ring(cur, 0x125000);
    assert(gcm_flip_into_fifo(CTX_EA, 2) == 1);
    assert(vm_read32(cur) == (GCM_FLIP_MARKER | 2u));
    assert(vm_read32(cur + 4) == (0x20000000u | RING_BEGIN));
    assert(vm_read32(CTX_EA + 8) == RING_BEGIN);
    s_title_overran = 0;
}

static void pacing_window_covers_only_first_flips_after_report(void)
{
    setenv("GCM_FLIP_MIN_MS", "60", 1);
    setenv("GCM_FLIP_PACE_WINDOW", "2", 1);
    assert(!gcm_flip_too_early());
    usleep(5000);
    assert(!gcm_flip_too_early());
    s_report_dma = CELL_GCM_CONTEXT_DMA_REPORT_LOCATION_MAIN;
    gcm_get_report((1u << 24) | 0x10u);
    assert(s_pace_window_open == 1);
    usleep(5000);
    assert(gcm_flip_too_early());
    usleep(70000);
    assert(!gcm_flip_too_early());
    usleep(5000);
    assert(gcm_flip_too_early());
    usleep(70000);
    assert(!gcm_flip_too_early());
    usleep(5000);
    assert(!gcm_flip_too_early());
    usleep(5000);
    assert(!gcm_flip_too_early());
}

int main(void)
{
    vm_base = calloc(1, VM_BYTES);
    assert(vm_base);
    ppu_vm_size = VM_BYTES;
    for (u32 page = 0; page < 65536; page++)
        s_io_address_table[page] = page < (VM_BYTES >> 20) ? (u16)page : 0xFFFF;
    for (u32 id = 0; id < 3; id++) s_display_buffer_set[id] = 1;
    flip_lands_at_current_without_recycling();
    flip_near_segment_end_stays_in_order();
    overran_title_still_recycles();
    pacing_window_covers_only_first_flips_after_report();
    puts("GCM flip-in-FIFO and pacing-window checks passed");
    return 0;
}
