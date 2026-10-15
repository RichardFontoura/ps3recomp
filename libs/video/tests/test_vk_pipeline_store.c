#include "../vulkan/rsx_vk_pipeline_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } \
                        checks++; } while (0)

static int checks;

static rsx_vks_entry make_entry(u64 vs, u64 fs, u32 topo, u32 depth_func)
{
    rsx_vks_entry e;
    memset(&e, 0, sizeof e);
    e.vs_key = vs;
    e.fs_key = fs;
    e.rs.depth_test = 1;
    e.rs.depth_func = depth_func;
    e.rs.alpha_ref_raw = 0x80;
    e.nslots = 4;
    e.stride = 64;
    e.rt_fmt = 37;
    e.rt_count = 1;
    e.topo_mask = topo;
    rsx_vks_entry_normalize(&e);
    return e;
}

static int test_pipeline_cache_header(void)
{
    u8 uuid[16], blob[64];
    for (int i = 0; i < 16; i++) uuid[i] = (u8)(i * 7 + 1);
    const u32 head[4] = { 32, 1, 0x10DE, 0x2684 };
    memset(blob, 0xAB, sizeof blob);
    memcpy(blob, head, sizeof head);
    memcpy(blob + 16, uuid, 16);
    REQUIRE(rsx_vks_pipeline_cache_valid(blob, sizeof blob, 0x10DE, 0x2684, uuid));
    REQUIRE(!rsx_vks_pipeline_cache_valid(blob, sizeof blob, 0x1002, 0x2684, uuid));
    REQUIRE(!rsx_vks_pipeline_cache_valid(blob, sizeof blob, 0x10DE, 0x2685, uuid));
    REQUIRE(!rsx_vks_pipeline_cache_valid(blob, 31, 0x10DE, 0x2684, uuid));
    REQUIRE(!rsx_vks_pipeline_cache_valid(NULL, 64, 0x10DE, 0x2684, uuid));
    u8 other[16];
    memcpy(other, uuid, 16);
    other[15] ^= 1;
    REQUIRE(!rsx_vks_pipeline_cache_valid(blob, sizeof blob, 0x10DE, 0x2684, other));
    u32 bad_version = 2;
    memcpy(blob + 4, &bad_version, 4);
    REQUIRE(!rsx_vks_pipeline_cache_valid(blob, sizeof blob, 0x10DE, 0x2684, uuid));
    return 0;
}

static int test_manifest(const char* dir)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/pipelines.bin", dir);
    rsx_vks_manifest m = { 0 };
    rsx_vks_entry a = make_entry(1, 2, 1u << 3, 0x0201);
    rsx_vks_entry b = make_entry(1, 3, 1u << 4, 0x0201);
    rsx_vks_entry a_strip = make_entry(1, 2, 1u << 4, 0x0201);
    a_strip.rs.alpha_ref_raw = 0x40;
    rsx_vks_entry_normalize(&a_strip);
    REQUIRE(rsx_vks_manifest_add(&m, &a) == 1);
    REQUIRE(rsx_vks_manifest_add(&m, &b) == 1);
    REQUIRE(rsx_vks_manifest_add(&m, &a_strip) == 1);
    REQUIRE(rsx_vks_manifest_add(&m, &a_strip) == 0);
    REQUIRE(m.count == 2);
    REQUIRE(m.entries[0].topo_mask == ((1u << 3) | (1u << 4)));
    REQUIRE(rsx_vks_manifest_save(path, &m) == 0);

    rsx_vks_manifest r = { 0 };
    REQUIRE(rsx_vks_manifest_load(path, &r) == 0);
    REQUIRE(r.count == 2);
    REQUIRE(rsx_vks_entry_same(&r.entries[0], &m.entries[0]));
    REQUIRE(rsx_vks_entry_same(&r.entries[1], &m.entries[1]));
    REQUIRE(r.entries[0].topo_mask == m.entries[0].topo_mask);
    REQUIRE(r.entries[1].topo_mask == (1u << 4));
    rsx_vks_entry c = make_entry(1, 2, 1u << 3, 0x0203);
    REQUIRE(rsx_vks_manifest_find(&r, &c) < 0);
    rsx_vks_manifest_free(&r);

    size_t n = 0;
    u8* raw = (u8*)rsx_vks_read_file(path, &n);
    REQUIRE(raw && n > 20);
    REQUIRE(rsx_vks_write_atomic(path, raw, n - 1) == 0);
    REQUIRE(rsx_vks_manifest_load(path, &r) != 0);
    rsx_vks_manifest_free(&r);
    u32 bad_version = 99;
    memcpy(raw + 4, &bad_version, 4);
    REQUIRE(rsx_vks_write_atomic(path, raw, n) == 0);
    REQUIRE(rsx_vks_manifest_load(path, &r) != 0);
    REQUIRE(r.count == 0);
    free(raw);
    rsx_vks_manifest_free(&r);
    rsx_vks_manifest_free(&m);
    remove(path);
    return 0;
}

static int test_spirv(const char* dir)
{
    u32 words[8] = { 0x07230203u, 0x00010000u, 0, 9, 0, 1, 2, 3 }, back[16];
    u32 n = 0;
    const u64 vk = rsx_vks_source_key("float4 main() : SV_Target { return 1; }", 1);
    REQUIRE(vk != rsx_vks_source_key("float4 main() : SV_Target { return 1; }", 0));
    REQUIRE(rsx_vks_spirv_load(dir, vk, back, 16, &n) != 0);
    REQUIRE(rsx_vks_spirv_store(dir, vk, words, 8) == 0);
    REQUIRE(rsx_vks_spirv_load(dir, vk, back, 16, &n) == 0);
    REQUIRE(n == 8 && memcmp(words, back, sizeof words) == 0);
    REQUIRE(rsx_vks_spirv_load(dir, vk, back, 4, &n) != 0);

    char path[1200];
    snprintf(path, sizeof path, "%s/spirv/%016llx.spv", dir, (unsigned long long)vk);
    words[0] = 0xDEADBEEFu;
    REQUIRE(rsx_vks_spirv_store(dir, vk, words, 8) == 0);
    REQUIRE(rsx_vks_spirv_load(dir, vk, back, 16, &n) != 0);
    REQUIRE(access(path, F_OK) != 0);
    return 0;
}

int main(int argc, char** argv)
{
    char base[1024];
    if (argc > 1) snprintf(base, sizeof base, "%s/vks-XXXXXX", argv[1]);
    else          snprintf(base, sizeof base, "/tmp/vks-XXXXXX");
    if (!mkdtemp(base)) { perror("mkdtemp"); return 1; }
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/cache", base);

    setenv("RSX_VK_SHADER_CACHE_DIR", dir, 1);
    setenv("RSX_VK_SHADER_CACHE", "0", 1);
    char got[1100];
    REQUIRE(rsx_vks_cache_dir(got, sizeof got) != 0);
    unsetenv("RSX_VK_SHADER_CACHE");
    REQUIRE(rsx_vks_cache_dir(got, sizeof got) == 0);
    REQUIRE(strcmp(got, dir) == 0);
    char spirv[1200];
    snprintf(spirv, sizeof spirv, "%s/spirv", dir);
    struct stat st;
    REQUIRE(stat(spirv, &st) == 0 && S_ISDIR(st.st_mode));

    if (test_pipeline_cache_header() || test_manifest(dir) || test_spirv(dir)) return 1;

    rmdir(spirv);
    rmdir(dir);
    rmdir(base);
    printf("vk pipeline store: %d checks passed\n", checks);
    return 0;
}
