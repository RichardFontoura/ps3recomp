#ifndef PS3RECOMP_RSX_VERTEX_CACHE_H
#define PS3RECOMP_RSX_VERTEX_CACHE_H

#include "rsx_vertex_compact.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_vcache rsx_vcache;

typedef struct rsx_vcache_region {
    u32 location;
    u32 offset;
    u32 bytes;
    u64 raw_off;
} rsx_vcache_region;

typedef struct rsx_vcache_output {
    const u8* verts;
    u32 n_verts;
    u32 stride;
    const u32* cuts;
    u32 n_cuts;
    const u32* remap;
    u32 n_source_refs;
    int refs_remapped;
} rsx_vcache_output;

typedef struct rsx_vcache_stats {
    u64 lookups;
    u64 hits;
    u64 misses_new;
    u64 misses_changed;
    u64 stores;
    u64 skipped;
    u64 evictions;
    u64 bytes_compared;
    u64 bytes_stored;
    u64 entries;
    u64 bytes_used;
} rsx_vcache_stats;

rsx_vcache* rsx_vcache_create(u64 budget_bytes);
void rsx_vcache_destroy(rsx_vcache* cache);
u64 rsx_vcache_hash(const u32* blob, u32 words);
int rsx_vcache_lookup(rsx_vcache* cache, const u32* blob, u32 words, u64 hash,
                      rsx_vertex_guest_ptr_fn read, void* user, rsx_vcache_output* out);
int rsx_vcache_want_store(rsx_vcache* cache, u64 hash);
int rsx_vcache_store(rsx_vcache* cache, const u32* blob, u32 words, u64 hash,
                     const rsx_vcache_region* regions, u32 n_regions,
                     const u8* raw, u64 raw_bytes, const rsx_vcache_output* out);
void rsx_vcache_get_stats(const rsx_vcache* cache, rsx_vcache_stats* out);

#ifdef __cplusplus
}
#endif

#endif
