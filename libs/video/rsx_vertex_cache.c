#include "rsx_vertex_cache.h"

#include <stdlib.h>
#include <string.h>

#define VC_BUCKETS            16384u
#define VC_SEEN               65536u
#define VC_BACKOFF_AFTER      4u
#define VC_BACKOFF_MAX_SHIFT  6u
#define VC_ALIGN              16u

typedef struct vc_entry {
    struct vc_entry* chain;
    struct vc_entry* older;
    struct vc_entry* newer;
    u64 hash;
    u64 raw_bytes;
    u64 footprint;
    u32 words;
    u32 n_regions;
    u32 fail_streak;
    u32 n_verts;
    u32 stride;
    u32 n_cuts;
    u32 n_source_refs;
    int refs_remapped;
    u32* blob;
    rsx_vcache_region* regions;
    u8* raw;
    u8* verts;
    u32* cuts;
    u32* remap;
} vc_entry;

struct rsx_vcache {
    vc_entry* buckets[VC_BUCKETS];
    vc_entry* newest;
    vc_entry* oldest;
    vc_entry* changed;
    u64 budget;
    rsx_vcache_stats stats;
    u64 seen[VC_SEEN];
};

static u64 vc_align(u64 n)
{
    return (n + VC_ALIGN - 1u) & ~(u64)(VC_ALIGN - 1u);
}

static void vc_lru_unlink(rsx_vcache* c, vc_entry* e)
{
    if (e->newer) e->newer->older = e->older; else c->newest = e->older;
    if (e->older) e->older->newer = e->newer; else c->oldest = e->newer;
    e->older = e->newer = NULL;
}

static void vc_lru_push(rsx_vcache* c, vc_entry* e)
{
    e->older = c->newest;
    e->newer = NULL;
    if (c->newest) c->newest->newer = e; else c->oldest = e;
    c->newest = e;
}

static vc_entry* vc_find(const rsx_vcache* c, const u32* blob, u32 words, u64 hash)
{
    for (vc_entry* e = c->buckets[hash & (VC_BUCKETS - 1u)]; e; e = e->chain)
        if (e->hash == hash && e->words == words && memcmp(e->blob, blob, (size_t)words * 4u) == 0)
            return e;
    return NULL;
}

static void vc_remove(rsx_vcache* c, vc_entry* e)
{
    vc_entry** link = &c->buckets[e->hash & (VC_BUCKETS - 1u)];
    while (*link != e) link = &(*link)->chain;
    *link = e->chain;
    vc_lru_unlink(c, e);
    if (c->changed == e) c->changed = NULL;
    c->stats.entries--;
    c->stats.bytes_used -= e->footprint;
    free(e);
}

rsx_vcache* rsx_vcache_create(u64 budget_bytes)
{
    rsx_vcache* c = (rsx_vcache*)calloc(1, sizeof *c);
    if (c) c->budget = budget_bytes;
    return c;
}

void rsx_vcache_destroy(rsx_vcache* cache)
{
    if (!cache) return;
    while (cache->oldest) vc_remove(cache, cache->oldest);
    free(cache);
}

u64 rsx_vcache_hash(const u32* blob, u32 words)
{
    u64 h = 0x9E3779B97F4A7C15ull ^ words;
    for (u32 i = 0; i < words; i++) {
        h ^= blob[i];
        h *= 0xff51afd7ed558ccdull;
        h ^= h >> 29;
    }
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h | 1u;
}

int rsx_vcache_lookup(rsx_vcache* cache, const u32* blob, u32 words, u64 hash,
                      rsx_vertex_guest_ptr_fn read, void* user, rsx_vcache_output* out)
{
    cache->stats.lookups++;
    cache->changed = NULL;
    vc_entry* e = vc_find(cache, blob, words, hash);
    if (!e) {
        cache->stats.misses_new++;
        return 0;
    }
    for (u32 i = 0; i < e->n_regions; i++) {
        const rsx_vcache_region* r = &e->regions[i];
        const u8* p = read(user, r->location, r->offset, r->bytes);
        cache->stats.bytes_compared += r->bytes;
        if (!p || memcmp(p, e->raw + r->raw_off, r->bytes) != 0) {
            cache->changed = e;
            cache->stats.misses_changed++;
            return 0;
        }
    }
    e->fail_streak = 0;
    vc_lru_unlink(cache, e);
    vc_lru_push(cache, e);
    out->verts = e->verts;
    out->n_verts = e->n_verts;
    out->stride = e->stride;
    out->cuts = e->cuts;
    out->n_cuts = e->n_cuts;
    out->remap = e->remap;
    out->n_source_refs = e->n_source_refs;
    out->refs_remapped = e->refs_remapped;
    cache->stats.hits++;
    return 1;
}

int rsx_vcache_want_store(rsx_vcache* cache, u64 hash)
{
    vc_entry* e = cache->changed;
    if (e && e->hash == hash) {
        const u32 f = ++e->fail_streak;
        if (f >= VC_BACKOFF_AFTER) {
            u32 shift = f - VC_BACKOFF_AFTER + 1u;
            if (shift > VC_BACKOFF_MAX_SHIFT) shift = VC_BACKOFF_MAX_SHIFT;
            if (f % (1u << shift)) {
                cache->stats.skipped++;
                return 0;
            }
        }
        return 1;
    }
    u64* slot = &cache->seen[hash & (VC_SEEN - 1u)];
    if (*slot == hash) return 1;
    *slot = hash;
    return 0;
}

int rsx_vcache_store(rsx_vcache* cache, const u32* blob, u32 words, u64 hash,
                     const rsx_vcache_region* regions, u32 n_regions,
                     const u8* raw, u64 raw_bytes, const rsx_vcache_output* out)
{
    const u64 verts_bytes = (u64)out->n_verts * out->stride;
    const u64 cut_bytes = (u64)out->n_cuts * 4u;
    const u64 remap_bytes = out->refs_remapped ? (u64)out->n_source_refs * 4u : 0u;
    const u64 size = vc_align(sizeof(vc_entry)) + vc_align((u64)words * 4u) +
                     vc_align((u64)n_regions * sizeof(rsx_vcache_region)) + vc_align(raw_bytes) +
                     vc_align(verts_bytes) + vc_align(cut_bytes) + vc_align(remap_bytes);
    if (size > cache->budget / 4u) {
        cache->stats.skipped++;
        return 0;
    }
    u32 fail_streak = 0;
    vc_entry* old = vc_find(cache, blob, words, hash);
    if (old) {
        fail_streak = old->fail_streak;
        vc_remove(cache, old);
    }
    while (cache->oldest && cache->stats.bytes_used + size > cache->budget) {
        vc_remove(cache, cache->oldest);
        cache->stats.evictions++;
    }
    u8* block = (u8*)malloc((size_t)size);
    if (!block) return 0;
    vc_entry* e = (vc_entry*)block;
    memset(e, 0, sizeof *e);
    u8* p = block + vc_align(sizeof(vc_entry));
    e->blob = (u32*)p;               p += vc_align((u64)words * 4u);
    e->regions = (rsx_vcache_region*)p; p += vc_align((u64)n_regions * sizeof(rsx_vcache_region));
    e->raw = p;                      p += vc_align(raw_bytes);
    e->verts = p;                    p += vc_align(verts_bytes);
    e->cuts = (u32*)p;               p += vc_align(cut_bytes);
    e->remap = remap_bytes ? (u32*)p : NULL;
    memcpy(e->blob, blob, (size_t)words * 4u);
    if (n_regions) memcpy(e->regions, regions, (size_t)n_regions * sizeof(rsx_vcache_region));
    if (raw_bytes) memcpy(e->raw, raw, (size_t)raw_bytes);
    if (verts_bytes) memcpy(e->verts, out->verts, (size_t)verts_bytes);
    if (cut_bytes) memcpy(e->cuts, out->cuts, (size_t)cut_bytes);
    if (remap_bytes) memcpy(e->remap, out->remap, (size_t)remap_bytes);
    e->hash = hash;
    e->words = words;
    e->n_regions = n_regions;
    e->raw_bytes = raw_bytes;
    e->footprint = size;
    e->fail_streak = fail_streak;
    e->n_verts = out->n_verts;
    e->stride = out->stride;
    e->n_cuts = out->n_cuts;
    e->n_source_refs = out->n_source_refs;
    e->refs_remapped = out->refs_remapped;
    vc_entry** bucket = &cache->buckets[hash & (VC_BUCKETS - 1u)];
    e->chain = *bucket;
    *bucket = e;
    vc_lru_push(cache, e);
    cache->stats.entries++;
    cache->stats.bytes_used += size;
    cache->stats.stores++;
    cache->stats.bytes_stored += raw_bytes + verts_bytes;
    return 1;
}

void rsx_vcache_get_stats(const rsx_vcache* cache, rsx_vcache_stats* out)
{
    if (cache) *out = cache->stats;
    else memset(out, 0, sizeof *out);
}
