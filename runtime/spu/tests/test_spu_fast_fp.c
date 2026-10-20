#include "spu_helpers.h"
#include <stdio.h>
#include <string.h>

typedef u128 (*op2_fn)(u128, u128);
typedef u128 (*op3_fn)(u128, u128, u128);

static uint64_t g_rng = 0x2545F4914F6CDD1Dull;
static int g_fail;
static long g_cases;

static uint64_t rnd64(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return g_rng;
}

static uint32_t rnd_float_bits(void)
{
    const uint32_t r = (uint32_t)(rnd64() >> 20);
    switch (r & 3u) {
    case 0: return r & 0x807FFFFFu;
    case 1: return (r & 0x807FFFFFu) | ((0x60u + (uint32_t)(rnd64() % 64u)) << 23);
    default: return (r & 0x807FFFFFu) | ((uint32_t)(rnd64() % 255u) << 23);
    }
}

static uint64_t rnd_double_bits(void)
{
    const uint64_t r = rnd64();
    switch (r & 3u) {
    case 0: return r & 0x800FFFFFFFFFFFFFull;
    case 1: return (r & 0x800FFFFFFFFFFFFFull) | ((0x3C0ull + rnd64() % 128u) << 52);
    default: return (r & 0x800FFFFFFFFFFFFFull) | ((rnd64() % 2047u) << 52);
    }
}

static u128 rnd_floats(void)
{
    u128 v;
    for (int i = 0; i < 4; i++) v._u32[i] = rnd_float_bits();
    return v;
}

static u128 rnd_doubles(void)
{
    u128 v;
    for (int i = 0; i < 2; i++) {
        const uint64_t d = rnd_double_bits();
        v._u32[i*2] = (uint32_t)(d >> 32);
        v._u32[i*2+1] = (uint32_t)d;
    }
    return v;
}

static void expect_same(const char* what, u128 got, u128 want)
{
    g_cases++;
    if (memcmp(&got, &want, sizeof got) == 0) return;
    if (g_fail++ < 16)
        printf("FAIL %s: %08X %08X %08X %08X != %08X %08X %08X %08X\n", what,
               got._u32[0], got._u32[1], got._u32[2], got._u32[3],
               want._u32[0], want._u32[1], want._u32[2], want._u32[3]);
}

static void check_simd_matches_ref(const char* mode)
{
    char what[64];
    for (int n = 0; n < 200000; n++) {
        const u128 a = rnd_floats(), b = rnd_floats();
        snprintf(what, sizeof what, "fa %s", mode);
        expect_same(what, spu_fa(a, b), spu_fa_ref(a, b));
        snprintf(what, sizeof what, "fs %s", mode);
        expect_same(what, spu_fs(a, b), spu_fs_ref(a, b));
    }
}

#if SPU_FAST_FP_SUPPORTED
static void check_double_op2(const char* what, op2_fn op)
{
    for (int n = 0; n < 50000; n++) {
        const u128 a = rnd_doubles(), b = rnd_doubles();
        const unsigned csr = _mm_getcsr();
        const u128 std = op(a, b);
        _mm_setcsr(csr | SPU_MXCSR_SPU_BITS);
        const u128 spu = op(a, b);
        _mm_setcsr(csr);
        expect_same(what, spu, std);
    }
}

static void check_double_op3(const char* what, op3_fn op)
{
    for (int n = 0; n < 50000; n++) {
        const u128 a = rnd_doubles(), b = rnd_doubles(), t = rnd_doubles();
        const unsigned csr = _mm_getcsr();
        const u128 std = op(a, b, t);
        _mm_setcsr(csr | SPU_MXCSR_SPU_BITS);
        const u128 spu = op(a, b, t);
        _mm_setcsr(csr);
        expect_same(what, spu, std);
    }
}

static u128 frds2(u128 a, u128 b) { (void)b; return spu_frds(a); }

static void check_spu_mode_semantics(void)
{
    const unsigned csr = _mm_getcsr();
    u128 a, b, want;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); memset(&want, 0, sizeof want);
    a._u32[0] = 0x3F800000u; b._u32[0] = 0x33C00000u;
    a._u32[1] = 0x00400000u; b._u32[1] = 0x00000000u;
    a._u32[2] = 0xBF800000u; b._u32[2] = 0xB3C00000u;
    a._u32[3] = 0x3F800000u; b._u32[3] = 0x3F800000u;
    __asm__ volatile("" : : "r"(&a), "r"(&b) : "memory");
    _mm_setcsr(csr | SPU_MXCSR_SPU_BITS);
    const u128 got = spu_fa(a, b);
    _mm_setcsr(csr);
    want._u32[0] = 0x3F800000u;
    want._u32[1] = 0x00000000u;
    want._u32[2] = 0xBF800000u;
    want._u32[3] = 0x40000000u;
    expect_same("fa truncates and flushes in SPU mode", got, want);
    const u128 rn = spu_fa(a, b);
    if (rn._u32[0] != 0x3F800001u) {
        g_fail++;
        printf("FAIL fa in standard mode should round to nearest: %08X\n", rn._u32[0]);
    }
}
#endif

int main(void)
{
    check_simd_matches_ref("standard");
#if SPU_FAST_FP_SUPPORTED
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(csr | SPU_MXCSR_SPU_BITS);
    check_simd_matches_ref("spu-mode");
    _mm_setcsr(csr);
    check_double_op2("dfa", spu_dfa);
    check_double_op2("dfs", spu_dfs);
    check_double_op2("dfm", spu_dfm);
    check_double_op2("dfceq", spu_dfceq);
    check_double_op2("dfcmeq", spu_dfcmeq);
    check_double_op2("dfcgt", spu_dfcgt);
    check_double_op2("dfcmgt", spu_dfcmgt);
    check_double_op2("frds", frds2);
    check_double_op3("dfma", spu_dfma);
    check_double_op3("dfms", spu_dfms);
    check_double_op3("dfnms", spu_dfnms);
    check_double_op3("dfnma", spu_dfnma);
    check_spu_mode_semantics();
#endif
    printf("test_spu_fast_fp: %ld cases, %d failures (fast fp %s)\n", g_cases, g_fail,
           SPU_FAST_FP_SUPPORTED ? "supported" : "not supported");
    return g_fail != 0;
}
