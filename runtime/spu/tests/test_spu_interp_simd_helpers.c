#include "../spu_helpers.h"
#include "../spu_interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef u128 (*ref_rr)(u128, u128);
typedef u128 (*ref_rrr)(u128, u128, u128);
typedef u128 (*ref_ri7)(u128, int);

static spu_context ctx;
static int g_fail;
static long g_cases;
static long g_nan_lanes;
static uint64_t g_rng = 0xD1B54A32D192ED03ull;

static const uint32_t k_edges[] = {
    0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu,
    0x00800000u, 0x3F800000u, 0xBF800000u, 0x7F7FFFFFu, 0xFF7FFFFFu, 0x7F800000u,
    0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0x3F7FFFFFu, 0x33800000u,
};

u128 spu_rdch(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
void spu_wrch(spu_context* c, uint32_t ch, u128 v) { (void)c; (void)ch; (void)v; abort(); }
uint32_t spu_rchcnt(spu_context* c, uint32_t ch) { (void)c; (void)ch; abort(); }
int g_spu_ls_watch_n, g_spu_ls_probe, g_wws_read_probe, g_wws_code_probe, g_spu_smc_watch;
spu_lifted_fn spu_lifted_lookup(const spu_context* c, uint32_t a) { (void)c; (void)a; return 0; }
void spu_spurs_taskset_syscall(spu_context* c) { (void)c; abort(); }
void spu_ls_watch_slow(uint32_t l, int w, const uint8_t* p, uint32_t pc, uint32_t lr) {
    (void)l; (void)w; (void)p; (void)pc; (void)lr; abort();
}

static uint32_t rnd32(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

static uint32_t rnd_operand(void)
{
    uint32_t r = rnd32();
    if ((r & 3u) == 0) return k_edges[rnd32() % (sizeof k_edges / sizeof k_edges[0])];
    if ((r & 3u) == 1) return r & 0x807FFFFFu;
    return (r & 0x807FFFFFu) | (((rnd32() % 60u) + 0x5Au) << 23);
}

static void be32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static u128 rnd128(void) { u128 r; for (int l = 0; l < 4; l++) r._u32[l] = rnd_operand(); return r; }

static int is_nan_bits(uint32_t u) { return (u & 0x7FFFFFFFu) > 0x7F800000u; }

static u128 run_one(uint32_t insn, u128 a, u128 b, u128 c)
{
    memset(&ctx, 0, sizeof ctx);
    spu_context_init(&ctx, 0);
    ctx.image_id = -1;
    ctx.gpr[10] = a; ctx.gpr[11] = b; ctx.gpr[12] = c;
    be32(ctx.ls + 0x100, insn);
    be32(ctx.ls + 0x104, 0);
    spu_interp_run(&ctx, 0x100);
    return ctx.gpr[20];
}

static void compare(const char* name, u128 got, u128 want, const u128* in, int nin)
{
    g_cases++;
    for (int l = 0; l < 4; l++) {
        if (got._u32[l] == want._u32[l]) continue;
        int nan_in = 0;
        for (int k = 0; k < nin; k++) nan_in |= is_nan_bits(in[k]._u32[l]);
        if (nan_in && is_nan_bits(got._u32[l]) && is_nan_bits(want._u32[l])) { g_nan_lanes++; continue; }
        if (g_fail++ < 8)
            printf("test_spu_interp_simd_helpers: %s lane %d got %08X want %08X (a %08X b %08X c %08X)\n",
                   name, l, got._u32[l], want._u32[l], in[0]._u32[l],
                   nin > 1 ? in[1]._u32[l] : 0, nin > 2 ? in[2]._u32[l] : 0);
        return;
    }
}

static uint32_t enc_rr(uint32_t op11) { return (op11 << 21) | (11u << 14) | (10u << 7) | 20u; }
static uint32_t enc_rrr(uint32_t op4) { return (op4 << 28) | (20u << 21) | (11u << 14) | (10u << 7) | 12u; }
static uint32_t enc_ri7(uint32_t op11, int i7) { return (op11 << 21) | ((uint32_t)(i7 & 0x7F) << 14) | (10u << 7) | 20u; }

int main(void)
{
    static const struct { const char* name; uint32_t op; ref_rr ref; } rr[] = {
        {"fm", 0x2C6, spu_fm_ref}, {"rotqby", 0x1DC, spu_rotqby_ref}, {"rotqmby", 0x1DD, spu_rotqmby_ref},
        {"shlqby", 0x1DF, spu_shlqby_ref}, {"rotqbybi", 0x1CC, spu_rotqbybi_ref},
        {"rotqmbybi", 0x1CD, spu_rotqmbybi_ref}, {"shlqbybi", 0x1CF, spu_shlqbybi_ref},
    };
    static const struct { const char* name; uint32_t op; ref_rrr ref; } rrr[] = {
        {"fma", 0xE, spu_fma_ref}, {"fms", 0xF, spu_fms_ref}, {"fnms", 0xD, spu_fnms_ref},
    };
    static const struct { const char* name; uint32_t op; ref_ri7 ref; } ri7[] = {
        {"rotqbyi", 0x1FC, spu_rotqbyi_ref}, {"rotqmbyi", 0x1FD, spu_rotqmbyi_ref}, {"shlqbyi", 0x1FF, spu_shlqbyi_ref},
    };
    for (int it = 0; it < 20000; it++) {
        u128 in[3] = {rnd128(), rnd128(), rnd128()};
        if (it & 1) in[1]._u32[0] = rnd32() & 0xFFu;
        for (unsigned k = 0; k < sizeof rr / sizeof rr[0]; k++)
            compare(rr[k].name, run_one(enc_rr(rr[k].op), in[0], in[1], in[2]), rr[k].ref(in[0], in[1]), in, 2);
        for (unsigned k = 0; k < sizeof rrr / sizeof rrr[0]; k++)
            compare(rrr[k].name, run_one(enc_rrr(rrr[k].op), in[0], in[1], in[2]), rrr[k].ref(in[0], in[1], in[2]), in, 3);
        int i7 = (int)(rnd32() % 128u) - 64;
        for (unsigned k = 0; k < sizeof ri7 / sizeof ri7[0]; k++)
            compare(ri7[k].name, run_one(enc_ri7(ri7[k].op, i7), in[0], in[1], in[2]), ri7[k].ref(in[0], i7), in, 1);
        compare("cntb", run_one(enc_rr(0x2B4), in[0], in[1], in[2]), spu_cntb_ref(in[0]), in, 0);
    }
    if (g_fail) {
        printf("test_spu_interp_simd_helpers: FAIL %d of %ld cases\n", g_fail, g_cases);
        return 1;
    }
    printf("test_spu_interp_simd_helpers: ok (%ld cases, %ld lanes with a NaN input differ only in NaN sign or payload)\n",
           g_cases, g_nan_lanes);
    return 0;
}
