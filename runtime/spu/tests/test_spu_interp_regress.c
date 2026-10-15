#include "../spu_helpers.h"
#include "../spu_interp.h"
#include <assert.h>
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

static void be32(uint8_t* p, uint32_t value) {
    p[0] = value >> 24; p[1] = value >> 16; p[2] = value >> 8; p[3] = value;
}

static void reset(void) {
    memset(&ctx, 0, sizeof(ctx));
    spu_context_init(&ctx, 0);
    ctx.image_id = -1;
}

static void expect(const char* name, int ok) {
    printf("%s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static uint32_t rr(uint32_t op11, uint32_t rt, uint32_t ra, uint32_t rb) {
    return (op11 << 21) | (rb << 14) | (ra << 7) | rt;
}

static void bisl_same_register(void) {
    reset();
    ctx.gpr[5] = spu_splat_u32(0x200);
    be32(ctx.ls + 0x100, rr(0x1A9, 5, 5, 0));
    be32(ctx.ls + 0x104, 0);
    be32(ctx.ls + 0x200, 0);
    spu_interp_run(&ctx, 0x100);
    expect("bisl $r5,$r5 branches to the old r5", ctx.status == SPU_STATUS_STOPPED_BY_STOP && ctx.pc == 0x204);
    expect("bisl $r5,$r5 leaves the link in r5", ctx.gpr[5]._u32[0] == 0x104);
}

static void mpyui(void) {
    reset();
    ctx.gpr[81]._u32[0] = 0x12340002; ctx.gpr[81]._u32[1] = 0x00000003;
    ctx.gpr[81]._u32[2] = 0xFFFF8000; ctx.gpr[81]._u32[3] = 0x0000FFFF;
    be32(ctx.ls + 0x100, (0x75u << 24) | ((0x3FFu & 0x3FF) << 14) | (81u << 7) | 80u);
    be32(ctx.ls + 0x104, 0);
    spu_interp_run(&ctx, 0x100);
    expect("mpyui $r80,$r81,-1 runs", ctx.status == SPU_STATUS_STOPPED_BY_STOP);
    expect("mpyui lanes", ctx.gpr[80]._u32[0] == 0x0001FFFEu && ctx.gpr[80]._u32[1] == 0x0002FFFDu &&
                          ctx.gpr[80]._u32[2] == 0x7FFF8000u && ctx.gpr[80]._u32[3] == 0xFFFE0001u);
}

static void mpyhha(void) {
    reset();
    ctx.gpr[80] = spu_splat_u32(10);
    ctx.gpr[81] = spu_splat_u32(0xFFFE0000u);
    ctx.gpr[82] = spu_splat_u32(0x00030000u);
    be32(ctx.ls + 0x100, rr(0x346, 80, 81, 82));
    be32(ctx.ls + 0x104, 0);
    spu_interp_run(&ctx, 0x100);
    expect("mpyhha accumulates into rt", ctx.status == SPU_STATUS_STOPPED_BY_STOP && ctx.gpr[80]._u32[0] == 4u);
}

static void rothmi_rotmahi(void) {
    reset();
    ctx.gpr[81] = spu_splat_u32(0x8000F000u);
    be32(ctx.ls + 0x100, (0x07Cu << 21) | ((uint32_t)(-4 & 0x7F) << 14) | (81u << 7) | 80u);
    be32(ctx.ls + 0x104, (0x07Eu << 21) | ((uint32_t)(-4 & 0x7F) << 14) | (81u << 7) | 79u);
    be32(ctx.ls + 0x108, 0);
    spu_interp_run(&ctx, 0x100);
    expect("rothmi/rotmahi run", ctx.status == SPU_STATUS_STOPPED_BY_STOP);
    expect("rothmi shifts logically", ctx.gpr[80]._u32[0] == 0x08000F00u);
    expect("rotmahi shifts arithmetically", ctx.gpr[79]._u32[0] == 0xF800FF00u);
}

int main(void) {
    bisl_same_register();
    mpyui();
    mpyhha();
    rothmi_rotmahi();
    printf("SPU interpreter regressions: %d failures\n", failures);
    return failures ? 1 : 0;
}
