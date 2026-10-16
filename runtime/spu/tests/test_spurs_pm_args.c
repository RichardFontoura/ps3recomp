#include "../spu_helpers.h"
#include "../spu_interp.h"
#include "../spurs_pm_args.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c; (void)ch; (void)v; abort(); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
int g_spu_ls_watch_n, g_spu_ls_probe, g_wws_read_probe, g_wws_code_probe, g_spu_smc_watch;
spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t a) { (void)c; (void)a; return 0; }
void spu_spurs_taskset_syscall(spu_context* c) { (void)c; abort(); }
void spu_ls_watch_slow(uint32_t l, int w, const uint8_t* p, uint32_t pc, uint32_t lr) {
    (void)l; (void)w; (void)p; (void)pc; (void)lr; abort();
}

static spu_context ctx;
static int failures;

static void be32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

static uint32_t rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void expect(const char* name, int ok) {
    printf("%s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static uint32_t rr(uint32_t op11, uint32_t rt, uint32_t ra, uint32_t rb) {
    return (op11 << 21) | (rb << 14) | (ra << 7) | rt;
}

static uint32_t ri7(uint32_t op11, uint32_t rt, uint32_t ra, uint32_t imm) {
    return (op11 << 21) | ((imm & 0x7F) << 14) | (ra << 7) | rt;
}

static uint32_t stqa(uint32_t rt, uint32_t lsa) {
    return (0x041u << 23) | (((lsa >> 2) & 0xFFFF) << 7) | rt;
}

static void run_entry(uint64_t wkl_data) {
    memset(&ctx, 0, sizeof(ctx));
    spu_context_init(&ctx, 0);
    ctx.image_id = -1;
    be32(ctx.ls + 0x100, ri7(0x1FF, 6, 4, 4));
    be32(ctx.ls + 0x104, stqa(6, 0x300));
    be32(ctx.ls + 0x108, rr(0x128, 4, 0, 0));
    be32(ctx.ls + 0x10C, stqa(4, 0x310));
    be32(ctx.ls + 0x110, 0);
    be32(ctx.ls + 0x200, 0);
    ctx.gpr[0] = spu_splat_u32(0x200);
    spurs_pm_set_wkl_arg(&ctx, wkl_data);
    spu_interp_run(&ctx, 0x100);
}

static void queue_without_status_area(void) {
    run_entry(0x0000000012070180ull);
    expect("claim copy carries the queue EA", rd32(ctx.ls + 0x300) == 0x12070180u);
    expect("attach is skipped when the high word is zero", ctx.pc == 0x204);
    expect("attach never sees the queue as its status area", rd32(ctx.ls + 0x310) == 0);
}

static void queue_with_status_area(void) {
    run_entry(0x157ACF8000A63500ull);
    expect("claim copy carries the low word", rd32(ctx.ls + 0x300) == 0x00A63500u);
    expect("attach runs when the high word is set", ctx.pc == 0x114);
    expect("attach reads the high word", rd32(ctx.ls + 0x310) == 0x157ACF80u);
}

int main(void) {
    queue_without_status_area();
    queue_with_status_area();
    printf("SPURS policy-module argument: %d failures\n", failures);
    return failures ? 1 : 0;
}
