#include "spu_regcache.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum { RC_UNBUILT = 0, RC_BUILDING = 1, RC_READY = 2 };

static const void** s_pairs;
static unsigned s_pair_count;
static unsigned s_pair_cap;
static const void** s_hash;
static uintptr_t s_mask;
static int s_active;
static atomic_int s_state;
static atomic_int s_ab_off;

static uintptr_t spu_rc_slot(const void* fn)
{
    uint64_t x = (uint64_t)(uintptr_t)fn;
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    return (uintptr_t)x;
}

static int spu_rc_wanted(void)
{
    const char* e = getenv("SPU_REGCACHE");
    if (e && e[0] == '0') return 0;
    if (getenv("SPU_LS_WATCH") || getenv("SPU_LS_LOWREAD") || getenv("SPU_SMC_WATCH")) return 0;
    return 1;
}

static void spu_rc_build(void)
{
    uintptr_t size = 1;
    while (size < (uintptr_t)s_pair_count * 4u) size <<= 1;
    const void** table = (const void**)calloc(size * 2u, sizeof *table);
    if (!table) return;
    for (unsigned i = 0; i < s_pair_count; i++) {
        const void* key = s_pairs[2u * i];
        uintptr_t j = spu_rc_slot(key) & (size - 1u);
        while (table[2u * j] && table[2u * j] != key) j = (j + 1u) & (size - 1u);
        table[2u * j] = key;
        table[2u * j + 1u] = s_pairs[2u * i + 1u];
    }
    s_mask = size - 1u;
    s_hash = table;
}

static void spu_rc_init(void)
{
    int expected = RC_UNBUILT;
    if (atomic_compare_exchange_strong(&s_state, &expected, RC_BUILDING)) {
        s_active = s_pair_count && spu_rc_wanted();
        if (s_active) spu_rc_build();
        if (!s_hash) s_active = 0;
        fprintf(stderr, "[SPU] register-cached lifted SPU code: %s (%u pairs; SPU_REGCACHE=0 turns it off)\n",
                s_active ? "on" : "off", s_pair_count);
        atomic_store_explicit(&s_state, RC_READY, memory_order_release);
        return;
    }
    while (atomic_load_explicit(&s_state, memory_order_acquire) != RC_READY) { }
}

void spu_rc_add_pairs(const void* const* pairs, unsigned n)
{
    if (atomic_load_explicit(&s_state, memory_order_acquire) != RC_UNBUILT) return;
    if (s_pair_count + n > s_pair_cap) {
        unsigned cap = s_pair_cap ? s_pair_cap : 4096u;
        while (cap < s_pair_count + n) cap *= 2u;
        const void** grown = (const void**)realloc((void*)s_pairs, (size_t)cap * 2u * sizeof *grown);
        if (!grown) return;
        s_pairs = grown;
        s_pair_cap = cap;
    }
    for (unsigned i = 0; i < n; i++) {
        s_pairs[2u * (s_pair_count + i)] = pairs[2u * i];
        s_pairs[2u * (s_pair_count + i) + 1u] = pairs[2u * i + 1u];
    }
    s_pair_count += n;
}

int spu_rc_active(void)
{
    if (atomic_load_explicit(&s_state, memory_order_acquire) != RC_READY) spu_rc_init();
    return s_active;
}

spu_rc_fn spu_rc_canonical(spu_rc_fn fn)
{
    if (!fn) return fn;
    if (atomic_load_explicit(&s_state, memory_order_acquire) != RC_READY) spu_rc_init();
    if (!s_active) return fn;
    const void* key = (const void*)fn;
    uintptr_t j = spu_rc_slot(key) & s_mask;
    for (;;) {
        const void* k = s_hash[2u * j];
        if (!k) return fn;
        if (k == key) return (spu_rc_fn)s_hash[2u * j + 1u];
        j = (j + 1u) & s_mask;
    }
}

spu_rc_fn spu_rc_select(spu_rc_fn fn)
{
    if (atomic_load_explicit(&s_ab_off, memory_order_relaxed)) return fn;
    return spu_rc_canonical(fn);
}

void spu_rc_set_enabled(int on)
{
    atomic_store_explicit(&s_ab_off, !on, memory_order_relaxed);
}
