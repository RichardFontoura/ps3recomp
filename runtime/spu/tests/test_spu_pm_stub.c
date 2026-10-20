#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t g_guest_mem[1u << 20];
uint8_t* vm_base = g_guest_mem;
uint32_t ppu_vm_size = 0;

#include "../spu_channels.c"

__attribute__((weak)) uint32_t cellGcm_flip_request_count(void) { return 0; }

#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); } } while (0)

enum { RETURN_TO = 0x2360 };

static int g_pass, g_fail;
static spu_context g_fast, g_slow;
static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
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

static uint32_t rnd32(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)g_rng;
}

static void put_word(spu_context* ctx, uint32_t lsa, uint32_t w)
{
    ctx->ls[lsa] = (uint8_t)(w >> 24);
    ctx->ls[lsa + 1] = (uint8_t)(w >> 16);
    ctx->ls[lsa + 2] = (uint8_t)(w >> 8);
    ctx->ls[lsa + 3] = (uint8_t)w;
}

static void setup(int save, uint32_t last_word)
{
    spu_context_init(&g_fast, 0);
    for (uint32_t i = 0; i < SPU_LS_SIZE; i += 4) put_word(&g_fast, i, rnd32());
    for (unsigned r = 0; r < 128; r++)
        for (unsigned k = 0; k < 4; k++) g_fast.gpr[r]._u32[k] = rnd32();
    for (uint32_t r = 0; r < SPU_PM_STUB_REGS; r++)
        put_word(&g_fast, SPU_PM_STUB_LSA + 4u * r, spu_pm_stub_word(save, r));
    put_word(&g_fast, SPU_PM_STUB_LSA + 4u * SPU_PM_STUB_REGS, last_word);
    g_fast.gpr[79]._u32[0] = RETURN_TO;
    g_fast.pc = SPU_PM_STUB_LSA;
    memcpy(&g_slow, &g_fast, sizeof g_fast);
}

static int same_state(void)
{
    return g_fast.pc == g_slow.pc &&
           memcmp(g_fast.gpr, g_slow.gpr, sizeof g_fast.gpr) == 0 &&
           memcmp(g_fast.ls, g_slow.ls, SPU_LS_SIZE) == 0;
}

static void run_both(void)
{
    s_spu_pm_stub = 1;
    g_spu_trampoline_fn = NULL;
    CHECK(spu_smc_microstep(&g_fast) == 1);
    CHECK(g_spu_trampoline_fn == spu_indirect_branch);
    s_spu_pm_stub = 0;
    g_spu_trampoline_fn = NULL;
    CHECK(spu_smc_microstep(&g_slow) == 1);
}

int main(void)
{
    for (int save = 0; save <= 1; save++) {
        setup(save, SPU_PM_STUB_BI79);
        CHECK(spu_pm_stub_kind(&g_fast) == save);
        run_both();
        CHECK(g_fast.pc == RETURN_TO);
        CHECK(same_state());
    }

    setup(0, 0x35002800u);
    CHECK(spu_pm_stub_kind(&g_fast) == -1);
    s_spu_pm_stub = 1;
    CHECK(spu_pm_stub_run(&g_fast) == 0);
    run_both();
    CHECK(same_state());

    setup(1, SPU_PM_STUB_BI79);
    put_word(&g_fast, SPU_PM_STUB_LSA + 4u * 40u, spu_pm_stub_word(0, 40));
    CHECK(spu_pm_stub_kind(&g_fast) == -1);

    printf("SPU PM context stub: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
