#include "../../runtime/trace/ps3_ab.h"
#include "../../runtime/spu/spu_regcache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_pass, g_fail;
static int g_calls_b, g_last_a = -1, g_toggles;
static unsigned g_frames;

static void check(int ok, const char* what)
{
    if (ok) { g_pass++; return; }
    g_fail++;
    printf("  FAIL: %s\n", what);
}

static void set_a(int on) { if (on != g_last_a) g_toggles++; g_last_a = on; }
static void set_b(int on) { (void)on; g_calls_b++; }
static unsigned frames(void) { return g_frames += 3u; }
static void orig_fn(spu_context* ctx) { (void)ctx; }
static void twin_fn(spu_context* ctx) { (void)ctx; }

int main(void)
{
    const ps3_ab_switch table[] = { { "alpha", set_a }, { "beta", set_b }, { "gamma-ray", set_b } };
    check(ps3_ab_parse("alpha", table, 3) == 1u, "one name selects its switch");
    check(ps3_ab_parse("beta,gamma-ray", table, 3) == 6u, "a comma list selects several");
    check(ps3_ab_parse("all", table, 3) == 7u, "all selects every switch");
    check(ps3_ab_parse("nope,alpha", table, 3) == 1u, "unknown names are ignored");
    check(ps3_ab_parse("", table, 3) == 0u, "an empty list selects nothing");

    unsetenv("PS3_AB");
    check(ps3_ab_start(table, 3, frames) == 0, "no PS3_AB, no A/B");
    setenv("PS3_AB", "alpha", 1);
    setenv("PS3_AB_SEC", "2", 1);
    check(ps3_ab_start(table, 3, frames) == 1, "PS3_AB starts the phases");
    struct timespec ts = { 4, 500000000 };
    nanosleep(&ts, NULL);
    check(g_toggles >= 2 && g_calls_b == 0, "only the chosen switch alternates");
    check(g_last_a == 1, "the third phase is A again (on)");

    static const void* pairs[2];
    pairs[0] = (const void*)orig_fn;
    pairs[1] = (const void*)twin_fn;
    spu_rc_add_pairs(pairs, 1);
    check(spu_rc_select(orig_fn) == twin_fn, "the register-cached twin is selected");
    spu_rc_set_enabled(0);
    check(spu_rc_select(orig_fn) == orig_fn, "phase B selects the original");
    check(spu_rc_canonical(orig_fn) == twin_fn && spu_rc_canonical(twin_fn) == twin_fn,
          "the canonical form ignores the phase");
    spu_rc_set_enabled(1);
    check(spu_rc_select(orig_fn) == twin_fn, "phase A selects the twin again");

    printf("in-run A/B: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail != 0;
}
