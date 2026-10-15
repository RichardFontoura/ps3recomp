#include "rsx_vk_pipeline_store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    u32 magic, version, translator, entry_size, count;
} vks_manifest_header;

typedef struct {
    u32 magic, translator, words, pad;
    u64 key;
} vks_spirv_header;

static int vks_mkdirs(const char* path)
{
    char buf[1024];
    const size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf) return -1;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] != '/' && buf[i] != '\0') continue;
        const char c = buf[i];
        buf[i] = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
        buf[i] = c;
    }
    return 0;
}

int rsx_vks_cache_dir(char* out, size_t size)
{
    const char* off = getenv("RSX_VK_SHADER_CACHE");
    if (off && off[0] == '0') return -1;
    const char* dir = getenv("RSX_VK_SHADER_CACHE_DIR");
    if (!dir || !*dir) return -1;
    if ((size_t)snprintf(out, size, "%s", dir) >= size) return -1;
    char spirv[1100];
    snprintf(spirv, sizeof spirv, "%s/spirv", out);
    return vks_mkdirs(spirv);
}

int rsx_vks_write_atomic(const char* path, const void* data, size_t size)
{
    char tmp[1100];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.%ld.tmp", path, (long)getpid()) >= sizeof tmp) return -1;
    FILE* f = fopen(tmp, "wb");
    if (!f) return -1;
    const int ok = fwrite(data, 1, size, f) == size;
    if (fflush(f) != 0 || fsync(fileno(f)) != 0 || fclose(f) != 0 || !ok) {
        remove(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

void* rsx_vks_read_file(const char* path, size_t* size)
{
    *size = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    void* buf = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long n = ftell(f);
        if (n > 0 && fseek(f, 0, SEEK_SET) == 0 && (buf = malloc((size_t)n)) != NULL) {
            if (fread(buf, 1, (size_t)n, f) == (size_t)n) *size = (size_t)n;
            else { free(buf); buf = NULL; }
        }
    }
    fclose(f);
    return buf;
}

u64 rsx_vks_source_key(const char* src, int stage)
{
    u64 h = 1469598103934665603ull;
    const u32 salt[2] = { RSX_VKS_TRANSLATOR_VERSION, (u32)stage };
    const u8* s = (const u8*)salt;
    for (size_t i = 0; i < sizeof salt; i++) { h ^= s[i]; h *= 1099511628211ull; }
    for (const u8* p = (const u8*)src; *p; p++) { h ^= *p; h *= 1099511628211ull; }
    return h ? h : 1u;
}

int rsx_vks_pipeline_cache_valid(const void* data, size_t size, u32 vendor_id,
                                 u32 device_id, const u8 uuid[16])
{
    if (!data || size < 32) return 0;
    u32 head[4];
    memcpy(head, data, sizeof head);
    if (head[0] < 32 || head[0] > size || head[1] != 1u) return 0;
    if (head[2] != vendor_id || head[3] != device_id) return 0;
    return memcmp((const u8*)data + 16, uuid, 16) == 0;
}

static void vks_spirv_path(char* out, size_t size, const char* dir, u64 key)
{
    snprintf(out, size, "%s/spirv/%016llx.spv", dir, (unsigned long long)key);
}

int rsx_vks_spirv_load(const char* dir, u64 key, u32* words, u32 max_words, u32* out_words)
{
    char path[1100];
    vks_spirv_path(path, sizeof path, dir, key);
    size_t n = 0;
    u8* data = (u8*)rsx_vks_read_file(path, &n);
    if (!data) return -1;
    vks_spirv_header h;
    int ok = n >= sizeof h;
    if (ok) {
        memcpy(&h, data, sizeof h);
        ok = h.magic == RSX_VKS_SPIRV_MAGIC && h.translator == RSX_VKS_TRANSLATOR_VERSION &&
             h.key == key && h.words > 0 && h.words <= max_words &&
             n == sizeof h + (size_t)h.words * 4u;
    }
    if (ok) {
        memcpy(words, data + sizeof h, (size_t)h.words * 4u);
        ok = words[0] == 0x07230203u;
    }
    free(data);
    if (!ok) { remove(path); return -1; }
    *out_words = h.words;
    return 0;
}

int rsx_vks_spirv_store(const char* dir, u64 key, const u32* words, u32 count)
{
    char path[1100];
    vks_spirv_path(path, sizeof path, dir, key);
    const size_t n = sizeof(vks_spirv_header) + (size_t)count * 4u;
    u8* buf = (u8*)malloc(n);
    if (!buf) return -1;
    vks_spirv_header h = { RSX_VKS_SPIRV_MAGIC, RSX_VKS_TRANSLATOR_VERSION, count, 0, key };
    memcpy(buf, &h, sizeof h);
    memcpy(buf + sizeof h, words, (size_t)count * 4u);
    const int r = rsx_vks_write_atomic(path, buf, n);
    free(buf);
    return r;
}

void rsx_vks_entry_normalize(rsx_vks_entry* e)
{
    e->rs.alpha_ref_raw = 0;
    e->rs.alpha_ref_format = 0;
}

int rsx_vks_entry_same(const rsx_vks_entry* a, const rsx_vks_entry* b)
{
    return a->vs_key == b->vs_key && a->fs_key == b->fs_key && a->nslots == b->nslots &&
           a->stride == b->stride && a->rt_fmt == b->rt_fmt && a->rt_count == b->rt_count &&
           a->cube_mask == b->cube_mask && memcmp(&a->rs, &b->rs, sizeof a->rs) == 0;
}

int rsx_vks_manifest_find(const rsx_vks_manifest* m, const rsx_vks_entry* e)
{
    for (u32 i = 0; i < m->count; i++)
        if (rsx_vks_entry_same(&m->entries[i], e)) return (int)i;
    return -1;
}

int rsx_vks_manifest_add(rsx_vks_manifest* m, const rsx_vks_entry* e)
{
    const int at = rsx_vks_manifest_find(m, e);
    if (at >= 0) {
        const u32 before = m->entries[at].topo_mask;
        m->entries[at].topo_mask |= e->topo_mask;
        return m->entries[at].topo_mask != before;
    }
    if (m->count == m->cap) {
        const u32 cap = m->cap ? m->cap * 2u : 256u;
        rsx_vks_entry* n = (rsx_vks_entry*)realloc(m->entries, (size_t)cap * sizeof *n);
        if (!n) return -1;
        m->entries = n;
        m->cap = cap;
    }
    m->entries[m->count++] = *e;
    return 1;
}

int rsx_vks_manifest_load(const char* path, rsx_vks_manifest* m)
{
    size_t n = 0;
    u8* data = (u8*)rsx_vks_read_file(path, &n);
    if (!data) return -1;
    vks_manifest_header h;
    int ok = n >= sizeof h;
    if (ok) {
        memcpy(&h, data, sizeof h);
        ok = h.magic == RSX_VKS_MANIFEST_MAGIC && h.version == RSX_VKS_FORMAT_VERSION &&
             h.translator == RSX_VKS_TRANSLATOR_VERSION && h.entry_size == sizeof(rsx_vks_entry) &&
             n == sizeof h + (size_t)h.count * sizeof(rsx_vks_entry);
    }
    if (ok) {
        const rsx_vks_entry* src = (const rsx_vks_entry*)(void*)(data + sizeof h);
        for (u32 i = 0; i < h.count; i++) {
            rsx_vks_entry e;
            memcpy(&e, &src[i], sizeof e);
            if (rsx_vks_manifest_add(m, &e) < 0) { ok = 0; break; }
        }
    }
    free(data);
    return ok ? 0 : -1;
}

int rsx_vks_manifest_save(const char* path, const rsx_vks_manifest* m)
{
    const size_t n = sizeof(vks_manifest_header) + (size_t)m->count * sizeof(rsx_vks_entry);
    u8* buf = (u8*)malloc(n);
    if (!buf) return -1;
    vks_manifest_header h = { RSX_VKS_MANIFEST_MAGIC, RSX_VKS_FORMAT_VERSION,
                              RSX_VKS_TRANSLATOR_VERSION, (u32)sizeof(rsx_vks_entry), m->count };
    memcpy(buf, &h, sizeof h);
    if (m->count) memcpy(buf + sizeof h, m->entries, (size_t)m->count * sizeof(rsx_vks_entry));
    const int r = rsx_vks_write_atomic(path, buf, n);
    free(buf);
    return r;
}

void rsx_vks_manifest_free(rsx_vks_manifest* m)
{
    free(m->entries);
    memset(m, 0, sizeof *m);
}
