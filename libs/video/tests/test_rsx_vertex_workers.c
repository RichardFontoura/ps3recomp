#include "rsx_vertex_workers.h"
#include "rsx_vertex_formats.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { VERTS = 20000, ROUNDS = 3000 };

static int g_pass, g_fail;
static u8 g_source[VERTS * 16];
static rsx_vertex_ref g_refs[VERTS];
static u8 g_serial[VERTS * 16];
static u8 g_parallel[VERTS * 16];

static void check(int ok, const char* what)
{
    if (ok) { g_pass++; return; }
    g_fail++;
    printf("  FAIL: %s\n", what);
}

static void plan_over_source(rsx_vertex_fetch_plan* plan)
{
    memset(plan, 0, sizeof *plan);
    plan->layout.mask = 1u;
    plan->layout.count = 1;
    plan->layout.stride = 16u;
    plan->layout.attrs[0] = 0;
    rsx_vertex_fetch_attr* a = &plan->attr[0];
    a->desc.type = RSX_VTX_TYPE_FLOAT;
    a->desc.size = 4;
    a->desc.stride = 16;
    a->elem_size = 16;
    a->stride = 16;
    a->stable_base = g_source;
    a->stable_first = 0;
    a->stable_last = VERTS - 1;
}

int main(void)
{
    u64 rng = 0x9E3779B97F4A7C15ull;
    for (u32 i = 0; i < sizeof g_source; i++) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        g_source[i] = (u8)(i % 4u == 0u ? 0x3Fu : rng);
    }
    for (u32 i = 0; i < VERTS; i++) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        g_refs[i].vertex_id = (u32)(rng % VERTS);
    }
    rsx_vertex_fetch_plan plan;
    plan_over_source(&plan);
    check(rsx_vertex_fetch_range(&plan, g_refs, 0, VERTS, g_serial, 16u), "the serial fetch succeeds");

    check(rsx_vertex_workers_start(4) == 4, "four workers start");
    int same = 1, ok = 1;
    for (u32 round = 0; round < ROUNDS; round++) {
        const u32 count = 1u + (u32)(round * 7919u % VERTS);
        const u32 chunk = 1u + round % 300u;
        memset(g_parallel, 0, (size_t)count * 16u);
        ok &= rsx_vertex_fetch_parallel(&plan, g_refs, count, g_parallel, 16u, chunk);
        same &= memcmp(g_parallel, g_serial, (size_t)count * 16u) == 0;
    }
    check(ok, "every parallel fetch succeeds");
    check(same, "back-to-back parallel jobs of every size give the serial bytes");

    rsx_vertex_fetch_plan broken = plan;
    broken.attr[0].stable_base = NULL;
    check(!rsx_vertex_fetch_parallel(&broken, g_refs, VERTS, g_parallel, 16u, 512u),
          "a vertex that cannot be read fails the whole parallel fetch");
    check(rsx_vertex_fetch_parallel(&plan, g_refs, VERTS, g_parallel, 16u, 512u) &&
          memcmp(g_parallel, g_serial, sizeof g_serial) == 0, "the next job is clean after a failure");

    rsx_vertex_workers_stop();
    check(rsx_vertex_workers_count() == 0, "the workers stop");
    check(rsx_vertex_fetch_parallel(&plan, g_refs, VERTS, g_parallel, 16u, 512u) &&
          memcmp(g_parallel, g_serial, sizeof g_serial) == 0, "without workers the fetch is serial");

    printf("RSX vertex workers: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
