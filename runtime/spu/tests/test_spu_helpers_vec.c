#include "spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_spu_ls_watch_n = 0;
void spu_ls_watch_slow(uint32_t lsa, int is_write, const uint8_t* p, uint32_t pc, uint32_t lr)
{
    (void)lsa; (void)is_write; (void)p; (void)pc; (void)lr;
}

#define REF1(n) static u128 ref_##n(u128 a) { return spu_##n(a); }
#define REF2(n) static u128 ref_##n(u128 a, u128 b) { return spu_##n(a, b); }
#define REF3(n) static u128 ref_##n(u128 a, u128 b, u128 c) { return spu_##n(a, b, c); }
#define REFI(n, T) static u128 ref_##n(u128 a, T i) { return spu_##n(a, i); }
#define REFS(n, T) static u128 ref_##n(T v) { return spu_##n(v); }

REF1(xsbh) REF1(xshw) REF1(fsm)
REF2(a) REF2(sf) REF2(ah) REF2(sfh) REF2(cg) REF2(bg)
REF2(and) REF2(or) REF2(xor) REF2(andc) REF2(orc) REF2(nor) REF2(nand) REF2(eqv)
REF2(ceq) REF2(ceqh) REF2(ceqb) REF2(cgt) REF2(clgt) REF2(clgth)
REF2(mpy) REF2(mpyu) REF2(mpyh) REF2(mpyhh) REF2(mpys)
REF2(shl) REF2(rotm) REF2(rotma) REF2(rot)
REF3(addx) REF3(sfx) REF3(selb) REF3(mpya)
REFI(ai, int32_t) REFI(sfi, int32_t) REFI(ahi, int32_t) REFI(sfhi, int32_t)
REFI(andi, int32_t) REFI(ori, int32_t) REFI(xori, int32_t)
REFI(andhi, int32_t) REFI(orhi, int32_t) REFI(xorhi, int32_t)
REFI(andbi, int32_t) REFI(orbi, int32_t) REFI(xorbi, int32_t)
REFI(ceqi, int32_t) REFI(ceqhi, int32_t) REFI(ceqbi, int32_t)
REFI(cgti, int32_t) REFI(cgthi, int32_t) REFI(cgtbi, int32_t)
REFI(clgti, int32_t) REFI(clgthi, int32_t) REFI(clgtbi, int32_t)
REFI(shli, int) REFI(shlhi, int) REFI(rotmi, int) REFI(rotmai, int)
REFI(rothmi, int) REFI(rotmhi, int) REFI(rotmahi, int) REFI(roti, int)
REFI(mpyi, int32_t) REFI(mpyui, int32_t) REFI(iohl, uint16_t)
REFS(splat_u32, uint32_t) REFS(il, int32_t) REFS(ila, uint32_t) REFS(ilhu, uint16_t)
REFS(ilh, uint16_t) REFS(link, uint32_t) REFS(pref_u32, uint32_t)

static u128 ref_ls_read(const spu_context* ctx, uint32_t lsa) { return spu_ls_read128(ctx, lsa); }
static void ref_ls_write(spu_context* ctx, uint32_t lsa, u128 v) { spu_ls_write128(ctx, lsa, v); }

#include "spu_helpers_vec.h"

#if defined(__SSE4_1__)

static uint64_t g_rng = 0x6A09E667F3BCC909ull;
static long g_cases;
static int g_fail;

static const uint32_t k_words[] = {
    0x00000000u, 0xFFFFFFFFu, 0x80000000u, 0x7FFFFFFFu, 0x00000001u, 0x00008000u, 0x00007FFFu,
    0xFFFF8000u, 0x0000FFFFu, 0xFFFF0000u, 0x00800080u, 0x7F7F7F7Fu, 0x80808080u, 0x0000001Fu,
    0x00000020u, 0x00000021u, 0x0000003Fu, 0x00000040u, 0xFFFFFFE0u, 0x0001FFFFu, 0x12345678u,
};
static const int32_t k_imms[] = {
    0, 1, -1, 2, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, -2, -7, -8, -15, -16, -17,
    -31, -32, -33, -63, -64, -65, -127, -128, 0x1FF, -0x200, 0x3FF, 0x7FFF, 0x8000, -0x8000,
    0xFFFF, 0x10000, 0x3FFFF, 0x7FFFFFFF, (int32_t)0x80000000u, 0x12345, -0x12345,
};

static uint32_t rnd32(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 11);
}

static uint32_t rnd_word(void)
{
    const uint32_t r = rnd32();
    return (r & 3u) == 0 ? k_words[rnd32() % (sizeof k_words / sizeof k_words[0])] : rnd32();
}

static u128 rnd_reg(void)
{
    u128 v;
    for (int i = 0; i < 4; i++) v._u32[i] = rnd_word();
    return v;
}

static int32_t rnd_imm(void)
{
    const uint32_t r = rnd32();
    if ((r & 1u) == 0) return k_imms[rnd32() % (sizeof k_imms / sizeof k_imms[0])];
    if ((r & 2u) == 0) return (int32_t)(rnd32() % 256u) - 128;
    return (int32_t)rnd32();
}

static void check(const char* name, u128 got, u128 want)
{
    g_cases++;
    if (memcmp(&got, &want, sizeof got) == 0) return;
    if (g_fail++ < 24)
        printf("FAIL %s: got %08X %08X %08X %08X want %08X %08X %08X %08X\n", name,
               got._u32[0], got._u32[1], got._u32[2], got._u32[3],
               want._u32[0], want._u32[1], want._u32[2], want._u32[3]);
}

#define T1(n) check(#n, spu__v_##n(a), ref_##n(a))
#define T2(n) check(#n, spu__v_##n(a, b), ref_##n(a, b))
#define T3(n) check(#n, spu__v_##n(a, b, c), ref_##n(a, b, c))
#define TI(n, T) check(#n, spu__v_##n(a, (T)imm), ref_##n(a, (T)imm))
#define TS(n, T) check(#n, spu__v_##n((T)w), ref_##n((T)w))

static void run_register_ops(void)
{
    for (int iter = 0; iter < 400000; iter++) {
        const u128 a = rnd_reg(), b = rnd_reg(), c = rnd_reg();
        const int32_t imm = rnd_imm();
        const uint32_t w = rnd_word();
        T1(xsbh); T1(xshw); T1(fsm);
        T2(a); T2(sf); T2(ah); T2(sfh); T2(cg); T2(bg);
        T2(and); T2(or); T2(xor); T2(andc); T2(orc); T2(nor); T2(nand); T2(eqv);
        T2(ceq); T2(ceqh); T2(ceqb); T2(cgt); T2(clgt); T2(clgth);
        T2(mpy); T2(mpyu); T2(mpyh); T2(mpyhh); T2(mpys);
#if defined(__AVX2__)
        T2(shl); T2(rotm); T2(rotma); T2(rot);
#endif
        T3(addx); T3(sfx); T3(selb); T3(mpya);
        TI(ai, int32_t); TI(sfi, int32_t); TI(ahi, int32_t); TI(sfhi, int32_t);
        TI(andi, int32_t); TI(ori, int32_t); TI(xori, int32_t);
        TI(andhi, int32_t); TI(orhi, int32_t); TI(xorhi, int32_t);
        TI(andbi, int32_t); TI(orbi, int32_t); TI(xorbi, int32_t);
        TI(ceqi, int32_t); TI(ceqhi, int32_t); TI(ceqbi, int32_t);
        TI(cgti, int32_t); TI(cgthi, int32_t); TI(cgtbi, int32_t);
        TI(clgti, int32_t); TI(clgthi, int32_t); TI(clgtbi, int32_t);
        TI(shli, int); TI(shlhi, int); TI(rotmi, int); TI(rotmai, int);
        TI(rothmi, int); TI(rotmhi, int); TI(rotmahi, int); TI(roti, int);
        TI(mpyi, int32_t); TI(mpyui, int32_t); TI(iohl, uint16_t);
        TS(splat_u32, uint32_t); TS(il, int32_t); TS(ila, uint32_t); TS(ilhu, uint16_t);
        TS(ilh, uint16_t); TS(link, uint32_t); TS(pref_u32, uint32_t);
    }
}

static void run_local_store(void)
{
    static uint8_t ls_ref[SPU_LS_SIZE], ls_vec[SPU_LS_SIZE];
    spu_context* ctx = (spu_context*)calloc(1, sizeof *ctx);
    for (uint32_t i = 0; i < SPU_LS_SIZE; i++) ls_ref[i] = ls_vec[i] = (uint8_t)rnd32();
    for (int iter = 0; iter < 200000; iter++) {
        const uint32_t lsa = rnd32();
        const u128 v = rnd_reg();
        ctx->ls = ls_ref;
        const u128 want = ref_ls_read(ctx, lsa);
        uint8_t* spu_rc_ls = ls_vec;
        check("ls_read", spu_ls_read128(ctx, lsa), want);
        ctx->ls = ls_ref;
        ref_ls_write(ctx, lsa, v);
        spu_ls_write128(ctx, lsa, v);
        g_cases++;
        if (memcmp(ls_ref, ls_vec, SPU_LS_SIZE) != 0 && g_fail++ < 24) printf("FAIL ls_write at 0x%05X\n", lsa);
    }
    free(ctx);
}

int main(void)
{
    run_register_ops();
    run_local_store();
    printf("test_spu_helpers_vec: %ld cases, %d failures\n", g_cases, g_fail);
    return g_fail != 0;
}

#else

int main(void)
{
    printf("test_spu_helpers_vec: built without SSE4.1, nothing to check\n");
    return 0;
}

#endif
