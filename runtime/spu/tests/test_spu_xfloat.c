#include "spu_helpers.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CHECK(c) do { if (c) ++passed; else { ++failed; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static int passed, failed;

static u128 words(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    u128 r;
    r._u32[0] = a; r._u32[1] = b; r._u32[2] = c; r._u32[3] = d;
    return r;
}

static u128 floats(float a, float b, float c, float d)
{
    u128 r;
    r._f32[0] = a; r._f32[1] = b; r._f32[2] = c; r._f32[3] = d;
    return r;
}

static int finite_lanes(u128 v)
{
    for (int i = 0; i < 4; i++)
        if (!isfinite(v._f32[i])) return 0;
    return 1;
}

int main(void)
{
    const u128 zero = spu_zero();
    const u128 ones = words(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
    const u128 big = words(0x7F800000u, 0xFF800000u, 0x7FFFFFFFu, 0xFFFFFFFFu);
    const u128 c = floats(1.5f, -2.0f, 3.0f, -4.25f);
    u128 r;

    r = spu_fnms(zero, ones, zero);
    CHECK(r._u32[0] == 0 && r._u32[1] == 0 && r._u32[2] == 0 && r._u32[3] == 0);
    CHECK((spu_gb(r)._u32[0] & 0xF) == 0);

    r = spu_fnms(ones, zero, c);
    CHECK(r._f32[0] == 1.5f && r._f32[1] == -2.0f && r._f32[2] == 3.0f && r._f32[3] == -4.25f);

    r = spu_fm(zero, big);
    CHECK(r._u32[0] == 0 && r._u32[1] == 0 && r._u32[2] == 0 && r._u32[3] == 0);
    r = spu_fm(big, zero);
    CHECK(r._u32[0] == 0 && r._u32[1] == 0 && r._u32[2] == 0 && r._u32[3] == 0);
    r = spu_fm(floats(2.0f, -3.0f, 0.5f, 8.0f), floats(4.0f, 2.0f, -6.0f, 0.125f));
    CHECK(r._f32[0] == 8.0f && r._f32[1] == -6.0f && r._f32[2] == -3.0f && r._f32[3] == 1.0f);

    r = spu_fma(zero, big, c);
    CHECK(r._f32[0] == 1.5f && r._f32[1] == -2.0f && r._f32[2] == 3.0f && r._f32[3] == -4.25f);
    r = spu_fma(big, zero, c);
    CHECK(r._f32[0] == 1.5f && r._f32[1] == -2.0f && r._f32[2] == 3.0f && r._f32[3] == -4.25f);
    r = spu_fma(floats(2.0f, 2.0f, 2.0f, 2.0f), floats(3.0f, -3.0f, 0.25f, 1.0f), c);
    CHECK(r._f32[0] == 7.5f && r._f32[1] == -8.0f && r._f32[2] == 3.5f && r._f32[3] == -2.25f);

    r = spu_fms(zero, big, c);
    CHECK(r._f32[0] == -1.5f && r._f32[1] == 2.0f && r._f32[2] == -3.0f && r._f32[3] == 4.25f);
    r = spu_fms(big, floats(1.0f, 1.0f, 1.0f, 1.0f), zero);
    CHECK(finite_lanes(r));
    CHECK(r._u32[0] == 0x7F7FFFFFu && r._u32[1] == 0xFF7FFFFFu && r._u32[2] == 0x7F7FFFFFu && r._u32[3] == 0xFF7FFFFFu);

    r = spu_fs(c, big);
    CHECK(finite_lanes(r));
    r = spu_fs(floats(5.0f, 1.0f, 0.0f, -1.0f), floats(2.0f, 4.0f, 0.0f, -1.0f));
    CHECK(r._f32[0] == 3.0f && r._f32[1] == -3.0f && r._f32[2] == 0.0f && r._f32[3] == 0.0f);

    r = spu_fcgt(words(0x7F800000u, 0x3F800000u, 0xBF800000u, 0x80000000u),
                 words(0x3F800000u, 0x7F800000u, 0xFFFFFFFFu, 0x00000000u));
    CHECK(r._u32[0] == 0xFFFFFFFFu && r._u32[1] == 0 && r._u32[2] == 0xFFFFFFFFu && r._u32[3] == 0);
    r = spu_fcgt(floats(1.0f, -1.0f, -2.0f, 3.0f), floats(-1.0f, 1.0f, -1.0f, 3.0f));
    CHECK(r._u32[0] == 0xFFFFFFFFu && r._u32[1] == 0 && r._u32[2] == 0 && r._u32[3] == 0);

    r = spu_fcmgt(words(0xFFFFFFFFu, 0x3F800000u, 0xC0000000u, 0x7F800000u),
                  words(0x7F800000u, 0xBF800000u, 0x3F800000u, 0x7FFFFFFFu));
    CHECK(r._u32[0] == 0xFFFFFFFFu && r._u32[1] == 0 && r._u32[2] == 0xFFFFFFFFu && r._u32[3] == 0);

    r = spu_fceq(words(0x7FFFFFFFu, 0x80000000u, 0x3F800000u, 0x7FC00000u),
                 words(0x7FFFFFFFu, 0x00000000u, 0x40000000u, 0x7FC00001u));
    CHECK(r._u32[0] == 0xFFFFFFFFu && r._u32[1] == 0xFFFFFFFFu && r._u32[2] == 0 && r._u32[3] == 0);

    r = spu_fcmeq(words(0xFFFFFFFFu, 0xBF800000u, 0x3F800000u, 0x40000000u),
                  words(0x7FFFFFFFu, 0x3F800000u, 0x3F800001u, 0xC0000000u));
    CHECK(r._u32[0] == 0xFFFFFFFFu && r._u32[1] == 0xFFFFFFFFu && r._u32[2] == 0 && r._u32[3] == 0xFFFFFFFFu);

    printf("SPU xfloat semantics: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
