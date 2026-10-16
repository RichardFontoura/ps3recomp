#include "../spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

static const uint32_t k_edges[] = {
    0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u, 0x007FFFFFu, 0x807FFFFFu,
    0x00800000u, 0x80800000u, 0x3F800000u, 0xBF800000u, 0x7F7FFFFFu, 0xFF7FFFFFu,
    0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u, 0x7FFFFFFFu,
    0xFFFFFFFFu, 0x7F000000u, 0x33800000u, 0x4B000000u, 0x3F7FFFFFu, 0x1F800000u,
};
static const int k_nedges = (int)(sizeof k_edges / sizeof k_edges[0]);

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static int g_fail;
static long g_cases;
static long g_nan_payload;

static uint32_t rnd32(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

static uint32_t rnd_float_bits(void)
{
    uint32_t r = rnd32();
    switch (r & 7u) {
    case 0: return k_edges[rnd32() % (uint32_t)k_nedges];
    case 1: return (r & 0x807FFFFFu) | (((rnd32() % 24u) + 0x70u) << 23);
    case 2: return r & 0x807FFFFFu;
    default: return (r & 0x807FFFFFu) | (((rnd32() % 60u) + 0x5Au) << 23);
    }
}

static int is_nan_bits(uint32_t u) { return (u & 0x7FFFFFFFu) > 0x7F800000u; }

static int lanes_match(u128 got, u128 want, u128 a, u128 b, u128 c)
{
    for (int l = 0; l < 4; l++) {
        if (got._u32[l] == want._u32[l]) continue;
        int nans = is_nan_bits(a._u32[l]) + is_nan_bits(b._u32[l]) + is_nan_bits(c._u32[l]);
        if (nans && is_nan_bits(got._u32[l]) && is_nan_bits(want._u32[l])) {
            g_nan_payload++;
            continue;
        }
        return 0;
    }
    return 1;
}

static void check(const char* name, u128 got, u128 want, u128 a, u128 b, u128 c)
{
    g_cases++;
    if (lanes_match(got, want, a, b, c)) return;
    if (g_fail++ < 8)
        printf("test_spu_simd_helpers: %s MISMATCH a=%08X %08X %08X %08X b=%08X %08X %08X %08X "
               "c=%08X %08X %08X %08X got=%08X %08X %08X %08X want=%08X %08X %08X %08X\n", name,
               a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0], b._u32[1], b._u32[2], b._u32[3],
               c._u32[0], c._u32[1], c._u32[2], c._u32[3], got._u32[0], got._u32[1], got._u32[2],
               got._u32[3], want._u32[0], want._u32[1], want._u32[2], want._u32[3]);
}

static void check_all(u128 a, u128 b, u128 c)
{
    check("fm", spu_fm(a, b), spu_fm_ref(a, b), a, b, c);
    check("fma", spu_fma(a, b, c), spu_fma_ref(a, b, c), a, b, c);
    check("fms", spu_fms(a, b, c), spu_fms_ref(a, b, c), a, b, c);
    check("fnms", spu_fnms(a, b, c), spu_fnms_ref(a, b, c), a, b, c);
}

static void check_bytes(const char* name, u128 got, u128 want, u128 a, u128 b)
{
    g_cases++;
    if (!memcmp(&got, &want, 16)) return;
    if (g_fail++ < 8)
        printf("test_spu_simd_helpers: %s MISMATCH a=%08X %08X %08X %08X b=%08X\n", name,
               a._u32[0], a._u32[1], a._u32[2], a._u32[3], b._u32[0]);
}

static void check_byte_moves(void)
{
#if defined(__SSSE3__)
    for (int v = 0; v < 256; v++) {
        u128 a, b = spu_zero();
        for (int l = 0; l < 16; l++) a._u8[l] = (uint8_t)(v + 37 * l);
        check_bytes("cntb", spu_cntb(a), spu_cntb_ref(a), a, b);
    }
    for (int it = 0; it < 200000; it++) {
        u128 a, b = spu_zero();
        for (int l = 0; l < 4; l++) a._u32[l] = rnd32();
        b._u32[0] = rnd32();
        for (int l = 1; l < 4; l++) b._u32[l] = rnd32();
        check_bytes("shlqby", spu_shlqby(a, b), spu_shlqby_ref(a, b), a, b);
        check_bytes("rotqby", spu_rotqby(a, b), spu_rotqby_ref(a, b), a, b);
        check_bytes("shlqbybi", spu_shlqbybi(a, b), spu_shlqbybi_ref(a, b), a, b);
        check_bytes("rotqbybi", spu_rotqbybi(a, b), spu_rotqbybi_ref(a, b), a, b);
        check_bytes("rotqmby", spu_rotqmby(a, b), spu_rotqmby_ref(a, b), a, b);
        check_bytes("rotqmbybi", spu_rotqmbybi(a, b), spu_rotqmbybi_ref(a, b), a, b);
        check_bytes("cntb", spu_cntb(a), spu_cntb_ref(a), a, b);
        b._u32[0] &= 0x3Fu;
        check_bytes("rotqmby", spu_rotqmby(a, b), spu_rotqmby_ref(a, b), a, b);
        b._u32[0] = (uint32_t)(-(int32_t)(b._u32[0] & 0x1Fu));
        check_bytes("rotqmby", spu_rotqmby(a, b), spu_rotqmby_ref(a, b), a, b);
        b._u32[0] = (uint32_t)(-(int32_t)((rnd32() & 0x1Fu) << 3));
        check_bytes("rotqmbybi", spu_rotqmbybi(a, b), spu_rotqmbybi_ref(a, b), a, b);
        b._u32[0] = rnd32() & 0x3Fu;
        check_bytes("shlqby", spu_shlqby(a, b), spu_shlqby_ref(a, b), a, b);
        check_bytes("rotqby", spu_rotqby(a, b), spu_rotqby_ref(a, b), a, b);
        b._u32[0] <<= 3;
        check_bytes("shlqbybi", spu_shlqbybi(a, b), spu_shlqbybi_ref(a, b), a, b);
        check_bytes("rotqbybi", spu_rotqbybi(a, b), spu_rotqbybi_ref(a, b), a, b);
        if (it < 64)
            for (int sh = -70; sh < 70; sh++) {
                b._u32[0] = (uint32_t)sh;
                check_bytes("shlqbyi", spu_shlqbyi(a, sh), spu_shlqbyi_ref(a, sh), a, b);
                check_bytes("rotqbyi", spu_rotqbyi(a, sh), spu_rotqbyi_ref(a, sh), a, b);
                check_bytes("rotqmbyi", spu_rotqmbyi(a, sh), spu_rotqmbyi_ref(a, sh), a, b);
            }
    }
#else
    puts("test_spu_simd_helpers: built without SSSE3, byte moves not compared");
#endif
}

static void check_fused(void)
{
#if defined(__FMA__) && defined(__AVX__)
    u128 a = spu_splat_u32(0x3F800800u), b = spu_splat_u32(0x3F800800u);
    u128 c = spu_splat_u32(0x3F801000u), negc = spu_splat_u32(0xBF801000u);
    const uint32_t want[4][2] = {
        {spu_fma(a, b, negc)._u32[0], 0x33800000u},
        {spu_fms(a, b, c)._u32[0], 0x33800000u},
        {spu_fnms(a, b, c)._u32[0], 0xB3800000u},
        {spu_fma_ref(a, b, negc)._u32[0], 0x33800000u},
    };
    const char* names[4] = {"fma fused", "fms fused", "fnms fused", "fma_ref fused"};
    for (int i = 0; i < 4; i++) {
        g_cases++;
        if (want[i][0] != want[i][1] && g_fail++ < 8)
            printf("test_spu_simd_helpers: %s got %08X want %08X\n", names[i], want[i][0], want[i][1]);
    }
    float fa = 1.0f + 1.0f / 4096.0f;
    volatile float prod = fa * fa;
    volatile float unfused = prod - (1.0f + 1.0f / 2048.0f);
    g_cases++;
    if (unfused != 0.0f && g_fail++ < 8)
        printf("test_spu_simd_helpers: directed case does not separate fused from unfused\n");
#endif
}

int main(void)
{
    check_fused();
    check_byte_moves();
#if !(defined(__FMA__) && defined(__AVX__))
    puts("test_spu_simd_helpers: built without AVX/FMA, float ops not compared");
    if (g_fail) {
        printf("test_spu_simd_helpers: FAIL %d of %ld cases\n", g_fail, g_cases);
        return 1;
    }
    printf("test_spu_simd_helpers: ok (%ld cases)\n", g_cases);
    return 0;
#else
    for (int i = 0; i < k_nedges; i++)
        for (int j = 0; j < k_nedges; j++)
            for (int k = 0; k < k_nedges; k += 4) {
                u128 a, b, c;
                for (int l = 0; l < 4; l++) {
                    a._u32[l] = k_edges[i];
                    b._u32[l] = k_edges[j];
                    c._u32[l] = k_edges[(k + l) % k_nedges];
                }
                check_all(a, b, c);
                for (int l = 0; l < 4; l++) {
                    a._u32[l] = k_edges[(i + l) % k_nedges];
                    b._u32[l] = k_edges[(j + 3 * l) % k_nedges];
                }
                check_all(a, b, c);
            }
    for (int it = 0; it < 2000000; it++) {
        u128 a, b, c;
        for (int l = 0; l < 4; l++) {
            a._u32[l] = rnd_float_bits();
            b._u32[l] = rnd_float_bits();
            c._u32[l] = rnd_float_bits();
        }
        check_all(a, b, c);
    }
    if (g_fail) {
        printf("test_spu_simd_helpers: FAIL %d of %ld cases\n", g_fail, g_cases);
        return 1;
    }
    printf("test_spu_simd_helpers: ok (%ld cases, %ld lanes with a NaN input differ only in NaN sign or payload)\n", g_cases, g_nan_payload);
    return 0;
#endif
}
