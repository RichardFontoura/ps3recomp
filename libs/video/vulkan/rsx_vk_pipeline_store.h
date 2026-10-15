#ifndef PS3RECOMP_RSX_VK_PIPELINE_STORE_H
#define PS3RECOMP_RSX_VK_PIPELINE_STORE_H

#include <stddef.h>

#include "../rsx_draw_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_vks_entry {
    u64                 vs_key, fs_key;
    rsx_be_render_state rs;
    u32                 nslots, stride, rt_fmt, rt_count;
    u32                 cube_mask, topo_mask;
} rsx_vks_entry;

typedef struct rsx_vks_manifest {
    rsx_vks_entry* entries;
    u32            count, cap;
} rsx_vks_manifest;

#define RSX_VKS_TRANSLATOR_VERSION 1u
#define RSX_VKS_MANIFEST_MAGIC     0x4D505747u
#define RSX_VKS_SPIRV_MAGIC        0x56535747u
#define RSX_VKS_FORMAT_VERSION     1u

int   rsx_vks_cache_dir(char* out, size_t size);
int   rsx_vks_write_atomic(const char* path, const void* data, size_t size);
void* rsx_vks_read_file(const char* path, size_t* size);

u64   rsx_vks_source_key(const char* src, int stage);
int   rsx_vks_pipeline_cache_valid(const void* data, size_t size, u32 vendor_id,
                                   u32 device_id, const u8 uuid[16]);

int   rsx_vks_spirv_load(const char* dir, u64 key, u32* words, u32 max_words, u32* out_words);
int   rsx_vks_spirv_store(const char* dir, u64 key, const u32* words, u32 count);

void  rsx_vks_entry_normalize(rsx_vks_entry* e);
int   rsx_vks_entry_same(const rsx_vks_entry* a, const rsx_vks_entry* b);
int   rsx_vks_manifest_find(const rsx_vks_manifest* m, const rsx_vks_entry* e);
int   rsx_vks_manifest_add(rsx_vks_manifest* m, const rsx_vks_entry* e);
int   rsx_vks_manifest_load(const char* path, rsx_vks_manifest* m);
int   rsx_vks_manifest_save(const char* path, const rsx_vks_manifest* m);
void  rsx_vks_manifest_free(rsx_vks_manifest* m);

#ifdef __cplusplus
}
#endif
#endif
