#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

#include "../spu_channels.c"

#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

enum { POLLS = 1000, FLIP_AT = 900, LABEL_EA = 0x40000, LSA = 0x3000, PUT_EA = 0x50000 };

static int g_pass, g_fail;
static spu_context g_ctx;
static uint32_t g_seen;
static int g_polls_done;
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi = 0;
int g_resv_store_active = 0;
uint32_t g_barrier_sync_watch = 0;
void (*g_spurs_kernel_hook)(uint32_t) = 0;
uint32_t g_ydkj_spurs_ctx_ea = 0;
uint32_t g_ydkj_real_spurs_ea = 0;
uint32_t g_ydkj_real_taskset_ea = 0;
uint32_t g_ydkj_real_taskid = 0;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;

void ppu_resv_break_store(uint64_t ea) { (void)ea; }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; }
unsigned long long ps3_ms_now(void) { return 0; }
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; }
void ydkj_wake_all_event_flags(void) { }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d) { (void)c; (void)a; (void)b; (void)d; }
void spu_thread_publish_ctx(uint32_t tid, void* ctx) { (void)tid; (void)ctx; }

static void wrch(spu_context* ctx, uint32_t channel, uint32_t v)
{
    spu_wrch(ctx, channel, spu_make_preferred_u32(v));
}

static void dma(spu_context* ctx, uint32_t lsa, uint32_t ea, uint32_t size, uint32_t cmd)
{
    wrch(ctx, MFC_LSA, lsa);
    wrch(ctx, MFC_EAH, 0);
    wrch(ctx, MFC_EAL, ea);
    wrch(ctx, MFC_Size, size);
    wrch(ctx, MFC_TagID, 0);
    wrch(ctx, MFC_Cmd, cmd);
}

static uint32_t ls_word(const spu_context* ctx, uint32_t lsa)
{
    const uint8_t* p = &ctx->ls[lsa];
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void set_label(uint32_t v)
{
    g_guest_mem[LABEL_EA + 0] = (uint8_t)(v >> 24);
    g_guest_mem[LABEL_EA + 1] = (uint8_t)(v >> 16);
    g_guest_mem[LABEL_EA + 2] = (uint8_t)(v >> 8);
    g_guest_mem[LABEL_EA + 3] = (uint8_t)v;
}

static void polling_job(spu_context* ctx)
{
    for (g_polls_done = 0; g_polls_done < POLLS; g_polls_done++) {
        if (g_polls_done == FLIP_AT) set_label(0x1234ABCDu);
        dma(ctx, LSA, LABEL_EA, 16, MFC_GET_CMD);
        g_seen = ls_word(ctx, LSA);
        if (g_seen == 0x1234ABCDu) return;
    }
}

static void put_spinning_job(spu_context* ctx)
{
    for (g_polls_done = 0; g_polls_done < POLLS; g_polls_done++)
        dma(ctx, LSA, PUT_EA, 16, MFC_PUT_CMD);
}

int main(void)
{
    spu_context_init(&g_ctx, 0);
    set_label(0);
    int halted = spu_run_with_halt(polling_job, &g_ctx);
    CHECK(halted == 0);
    CHECK(g_seen == 0x1234ABCDu);
    CHECK(g_polls_done == FLIP_AT);

    spu_context_init(&g_ctx, 0);
    halted = spu_run_with_halt(put_spinning_job, &g_ctx);
    CHECK(halted == 1);
    CHECK(g_polls_done < POLLS);

    printf("SPU DMA poll: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
