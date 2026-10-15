#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

#include "../spu_channels.c"

#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

enum { REGION = 0x80, ENTRY = 0x20, LOADED_AT = 0x12340 };

static int g_pass, g_fail;
static spu_context g_ctx;
static uint8_t g_region[REGION];
static int g_calls;
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

static void fragment_entry(spu_context* ctx)
{
    g_calls++;
    ctx->gpr[3]._u32[0] = SPU_PIC(ctx, 0x40);
}

static const spu_pic_entry g_entries[] = {
    { ENTRY, 0x10, 0x60, fragment_entry },
    { 0, 0, 0, NULL },
};

int main(void)
{
    for (unsigned i = 0; i < REGION; i++) g_region[i] = (uint8_t)(i * 37u + 11u);
    spu_pic_register(g_entries, 1, g_region, REGION, 0);

    spu_context_init(&g_ctx, 0);
    memcpy(g_ctx.ls + LOADED_AT, g_region, REGION);
    g_ctx.pc = LOADED_AT + ENTRY;
    spu_fn fn = spu_pic_lookup(&g_ctx);
    CHECK(fn == fragment_entry);
    CHECK(g_ctx.pic_base == LOADED_AT);
    fn(&g_ctx);
    CHECK(g_calls == 1);
    CHECK(g_ctx.gpr[3]._u32[0] == LOADED_AT + 0x40);

    g_ctx.pc = LOADED_AT + ENTRY + 4;
    CHECK(spu_pic_lookup(&g_ctx) == NULL);

    g_ctx.ls[LOADED_AT + 0x50] ^= 0xFF;
    g_ctx.pc = LOADED_AT + ENTRY;
    CHECK(spu_pic_lookup(&g_ctx) == NULL);
    g_ctx.ls[LOADED_AT + 0x50] ^= 0xFF;

    g_ctx.ls[LOADED_AT + 0x70] ^= 0xFF;
    CHECK(spu_pic_lookup(&g_ctx) == fragment_entry);

    spu_context_init(&g_ctx, 0);
    memcpy(g_ctx.ls + 0x3FF80, g_region, REGION);
    g_ctx.pc = 0x3FF80 + ENTRY;
    CHECK(spu_pic_lookup(&g_ctx) == fragment_entry);
    CHECK(SPU_PIC(&g_ctx, 0x100) == ((0x3FF80u + 0x100u) & SPU_LS_MASK));

    printf("SPU PIC dispatch: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
