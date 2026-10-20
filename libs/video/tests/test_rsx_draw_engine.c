/*
 * ps3recomp - the platform-neutral halves of the RSX draw engine
 *
 * Everything here runs with no GPU and no host graphics API: the engine is
 * driven through rsx_draw_engine_method(), exactly as the FIFO walker drives
 * it, against a stub backend that records what it was asked for. That is the
 * only way to check the four pieces whose bugs are silent -- a surface that is
 * reallocated when it should not be, a texture unit that samples guest memory
 * instead of the render target at the same address, a texture that never
 * updates because its content hash is only taken once, and a pipeline that is
 * recompiled every time a constant changes.
 *
 * Builds and runs on Linux as well as macOS; nothing in it is Apple-specific.
 */
#include "rsx_draw_engine.h"
#include "rsx_primitives.h"   /* RSX_PRIMITIVE_* */
#include "rsx_vertex_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (cond) {                                                           \
            printf("[PASS] " __VA_ARGS__); printf("\n");                      \
        } else {                                                              \
            printf("[FAIL] " __VA_ARGS__);                                    \
            printf("   (%s:%d)\n", __FILE__, __LINE__);                       \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

/* ---- the guest memory the engine reads ---------------------------------- */
/*
 * Two number spaces, as on the hardware: an RSX offset means one thing in the
 * IO-mapped main-memory context and another in local video memory. The test's
 * map puts main at the bottom of the buffer and local above it, which is what
 * lets the surface-keying check bind two different objects at one offset.
 */
#define GUEST_BYTES     (16u << 20)
#define GUEST_LOCAL_EA  (8u << 20)

u8* vm_base;
u32 ppu_vm_size;

u32 cellGcmResolveLocated(int local, u32 offset)
{
    return local ? GUEST_LOCAL_EA + offset : offset;
}

u32 cellGcmResolveIO(u32 offset)
{
    return offset < GUEST_LOCAL_EA ? offset : 0;
}

/* No cellGcmSys here: nothing was registered before the engine came up. */
void cellGcm_replay_display_buffers_to_engine(void) {}

/* ---- the stub backend ---------------------------------------------------- */

#define STUB_MAX_OBJECTS 8192
/* Colour clears kept by surface, so a test can say which targets were hit. */
#define STUB_MAX_CLEARED 8

typedef struct {
    u32 next_handle;
    u8  alive[STUB_MAX_OBJECTS];

    int n_texture_create, n_texture_release, n_texture_upload;
    int n_texture_live, texture_live_cap;
    int n_color_create, n_color_release, n_color_seeded;
    int n_depth_create, n_depth_snapshot;
    int n_pipeline_create;
    int n_draw, n_clear_color, n_clear_ds, n_present;
    int n_clear_rect, n_clear_ds_rect, n_region, n_surface_copy;
    u32 clear_rect[4], copy_size[2], region_size[2];
    u32 cleared[STUB_MAX_CLEARED];

    /* the last draw's bindings */
    u32 bound_tex[RSX_BE_MAX_TEXTURES];
    u32 bound_mask;
    u32 bound_rt[RSX_BE_MAX_COLOR_TARGETS];
    u32 bound_rt_count;
    u32 pipeline_rt_count;
    u32 bound_surface, bound_depth, bound_pipeline;
    u32 draw_vertices, draw_indices, draw_stride;
    float draw_first_vertex[4];   /* ATTR0 of the first vertex handed over */
    rsx_topology draw_topology;
    u32 scissor[4];
    u32 stencil_ref;
    u32 last_created_texture, last_created_color;
    u32 last_released;
    float last_clear_depth;
    int n_depth_readback;
    float ps_tail[4];
} StubState;

static StubState stub;

static u32 stub_handle(void)
{
    if (stub.next_handle + 1 >= STUB_MAX_OBJECTS) return 0;
    const u32 h = ++stub.next_handle;
    stub.alive[h] = 1;
    return h;
}

static int stub_init(void* u, u32 w, u32 h) { (void)u; (void)w; (void)h; return 0; }
static void stub_shutdown(void* u) { (void)u; }
static void stub_submit(void* u, u32 reason) { (void)u; (void)reason; }

static u32 stub_texture_create(void* u, rsx_be_format f, u32 w, u32 h, u32 mips,
                               u32 faces, u32 remap, u32 rsx_fmt)
{
    (void)u; (void)f; (void)w; (void)h; (void)mips; (void)faces;
    (void)remap; (void)rsx_fmt;
    stub.n_texture_create++;
    if (stub.texture_live_cap && stub.n_texture_live >= stub.texture_live_cap) return 0;
    stub.last_created_texture = stub_handle();
    if (stub.last_created_texture) stub.n_texture_live++;
    return stub.last_created_texture;
}

static void stub_texture_upload(void* u, u32 t, u32 face, u32 mip, u32 w, u32 h,
                                const void* src, u32 row, u32 rows)
{
    (void)u; (void)t; (void)face; (void)mip; (void)w; (void)h;
    (void)src; (void)row; (void)rows;
    stub.n_texture_upload++;
}

static void stub_release(void* u, u32 handle)
{
    (void)u;
    if (handle && handle < STUB_MAX_OBJECTS) stub.alive[handle] = 0;
    stub.last_released = handle;
}

static void stub_texture_release(void* u, u32 handle)
{
    stub.n_texture_release++;
    if (handle && handle < STUB_MAX_OBJECTS && stub.alive[handle]) stub.n_texture_live--;
    stub_release(u, handle);
}

static u32 stub_color_create(void* u, rsx_be_format f, u32 w, u32 h,
                             const void* seed, u32 seed_row)
{
    (void)u; (void)f; (void)w; (void)h; (void)seed_row;
    stub.n_color_create++;
    if (seed) stub.n_color_seeded++;
    stub.last_created_color = stub_handle();
    return stub.last_created_color;
}

static void stub_color_release(void* u, u32 handle)
{ stub.n_color_release++; stub_release(u, handle); }

static u32 stub_surface_view(void* u, u32 surface, u32 remap, u32 fmt)
{ (void)u; (void)remap; (void)fmt; return surface; }

static u32 stub_depth_create(void* u, u32 w, u32 h)
{ (void)u; (void)w; (void)h; stub.n_depth_create++; return stub_handle(); }

static u32 stub_depth_snapshot(void* u, u32 d, u32 w, u32 h)
{ (void)u; (void)d; (void)w; (void)h; stub.n_depth_snapshot++; return stub_handle(); }

static u32 stub_pipeline_create(void* u, const char* vs, const char* ps,
                                const rsx_be_render_state* rs,
                                const rsx_vertex_layout_plan* layout,
                                u32 stride, rsx_be_format rt, u32 rt_count)
{
    (void)u; (void)rs; (void)layout; (void)stride; (void)rt;
    if (!vs || !ps || !*vs || !*ps) return 0;
    stub.n_pipeline_create++;
    stub.pipeline_rt_count = rt_count;
    return stub_handle();
}

static void stub_pipeline_release(void* u, u32 p) { (void)u; (void)p; }

static void stub_bind_targets(void* u, const u32* s, u32 count, u32 d)
{
    (void)u;
    stub.bound_surface = count ? s[0] : 0;
    stub.bound_rt_count = count;
    for (u32 i = 0; i < RSX_BE_MAX_COLOR_TARGETS; i++)
        stub.bound_rt[i] = (i < count) ? s[i] : 0;
    stub.bound_depth = d;
}
static void stub_bind_pipeline(void* u, u32 p) { (void)u; stub.bound_pipeline = p; }
static void stub_bind_vs(void* u, const void* d, u32 n) { (void)u; (void)d; (void)n; }
static void stub_bind_ps(void* u, const void* d, u32 n)
{
    (void)u;
    if (d && n >= sizeof stub.ps_tail)
        memcpy(stub.ps_tail, (const u8*)d + n - sizeof stub.ps_tail, sizeof stub.ps_tail);
}

static void stub_bind_textures(void* u, const u32* tex,
                               const rsx_be_sampler_desc* smp, u32 mask)
{
    (void)u; (void)smp;
    memcpy(stub.bound_tex, tex, sizeof stub.bound_tex);
    stub.bound_mask = mask;
}

static void stub_bind_vtextures(void* u, const u32* tex,
                                const rsx_be_sampler_desc* smp, u32 mask)
{ (void)u; (void)tex; (void)smp; (void)mask; }

static void stub_viewport(void* u, float x, float y, float w, float h)
{ (void)u; (void)x; (void)y; (void)w; (void)h; }

static void stub_scissor(void* u, u32 x, u32 y, u32 w, u32 h)
{
    (void)u;
    stub.scissor[0] = x; stub.scissor[1] = y;
    stub.scissor[2] = w; stub.scissor[3] = h;
}

static void stub_stencil_ref(void* u, u32 r) { (void)u; stub.stencil_ref = r; }

static u64 g_draw_hash;

static u64 fnv64(u64 h, const void* data, u64 bytes)
{
    const u8* p = (const u8*)data;
    for (u64 i = 0; i < bytes; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static void stub_draw(void* u, rsx_topology topology, const void* verts,
                      u32 count, u32 stride, const u32* indices,
                      u32 index_count)
{
    (void)u;
    g_draw_hash = fnv64(0xCBF29CE484222325ull, &count, sizeof count);
    g_draw_hash = fnv64(g_draw_hash, &index_count, sizeof index_count);
    if (verts) g_draw_hash = fnv64(g_draw_hash, verts, (u64)count * stride);
    if (indices) g_draw_hash = fnv64(g_draw_hash, indices, (u64)index_count * 4u);
    stub.n_draw++;
    memset(stub.draw_first_vertex, 0, sizeof stub.draw_first_vertex);
    if (verts && stride >= sizeof stub.draw_first_vertex)
        memcpy(stub.draw_first_vertex, verts, sizeof stub.draw_first_vertex);
    stub.draw_topology = topology;
    stub.draw_vertices = count;
    stub.draw_stride = stride;
    stub.draw_indices = index_count;
}

static void stub_clear_color(void* u, u32 s, const float rgba[4])
{
    (void)u; (void)rgba;
    if (stub.n_clear_color < (int)STUB_MAX_CLEARED)
        stub.cleared[stub.n_clear_color] = s;
    stub.n_clear_color++;
}

static void stub_clear_ds(void* u, u32 d, u32 f, float z, u8 s)
{ (void)u; (void)d; (void)f; (void)s; stub.n_clear_ds++; stub.last_clear_depth = z; }

static void stub_clear_rect(void* u, u32 s, u32 x, u32 y, u32 w, u32 h, const float rgba[4])
{
    (void)u; (void)s; (void)rgba;
    stub.n_clear_rect++;
    stub.clear_rect[0] = x; stub.clear_rect[1] = y;
    stub.clear_rect[2] = w; stub.clear_rect[3] = h;
}

static void stub_clear_ds_rect(void* u, u32 d, u32 f, float z, u8 st, u32 x, u32 y, u32 w, u32 h)
{
    (void)u; (void)d; (void)f; (void)st; (void)x; (void)y; (void)w; (void)h;
    stub.n_clear_ds_rect++;
    stub.last_clear_depth = z;
}

static u32 stub_surface_region(void* u, u32 surface, u32 w, u32 h, u32 remap, u32 fmt)
{
    (void)u; (void)remap; (void)fmt;
    stub.n_region++;
    stub.region_size[0] = w; stub.region_size[1] = h;
    return surface | 0x40000000u;
}

static void stub_surface_copy(void* u, u32 dst, u32 src, u32 w, u32 h)
{
    (void)u; (void)dst; (void)src;
    stub.n_surface_copy++;
    stub.copy_size[0] = w; stub.copy_size[1] = h;
}

static u32 presented_surface;
static void stub_present(void* u, u32 s) { (void)u; presented_surface = s; stub.n_present++; }

/* One pixel of a surface, in the R,G,B,A order rsx_texture_decode produces:
 * the readback hook has to put those back into the guest's A8R8G8B8. */
static u8 g_stub_pixel[4] = { 0x11, 0x22, 0x33, 0x44 };
static int g_readbacks;
static struct { int n; u32 texture, target, depth_bytes, pitch, w, h, base_format; } g_from_target;
static struct { int n; u32 dst, dx, dy, src, sx, sy, w, h; } g_blit;

static void stub_readback(void* u, u32 s, u32 x, u32 y, u32 w, u32 h,
                          void* out, u32 pitch)
{
    (void)u; (void)s; (void)x; (void)y;
    g_readbacks++;
    for (u32 r = 0; r < h; r++)
        for (u32 c = 0; c < w; c++)
            memcpy((u8*)out + (size_t)r * pitch + c * 4u, g_stub_pixel, 4);
}

/* Every depth target reads back as 0.5, which Z24S8 stores as 0x800000. */
static int stub_depth_readback(void* u, u32 d, u32 x, u32 y, u32 w, u32 h,
                               float* out, u32 pitch)
{
    (void)u; (void)d; (void)x; (void)y;
    stub.n_depth_readback++;
    for (u32 r = 0; r < h; r++)
        for (u32 c = 0; c < w; c++)
            *(float*)((u8*)out + (size_t)r * pitch + c * 4u) = 0.5f;
    return 0;
}

/* The last rectangle pushed into a colour target, and its first pixel. */
static struct { int n; u32 surface, x, y, w, h; u8 first[4]; } g_written;

static void stub_color_write(void* u, u32 s, u32 x, u32 y, u32 w, u32 h,
                             const void* rows, u32 row_bytes)
{
    (void)u; (void)row_bytes;
    g_written.n++;
    g_written.surface = s; g_written.x = x; g_written.y = y;
    g_written.w = w; g_written.h = h;
    memcpy(g_written.first, rows, 4);
}

static int stub_texture_from_target(void* u, u32 texture, u32 target, u32 depth_bytes,
                                    u32 pitch, u32 w, u32 h, const rsx_tex_layout* tl)
{
    (void)u;
    g_from_target.n++;
    g_from_target.texture = texture; g_from_target.target = target;
    g_from_target.depth_bytes = depth_bytes; g_from_target.pitch = pitch;
    g_from_target.w = w; g_from_target.h = h; g_from_target.base_format = tl->base_format;
    return 1;
}

static int stub_surface_blit(void* u, u32 dst, u32 dx, u32 dy, u32 src, u32 sx, u32 sy,
                             u32 w, u32 h)
{
    (void)u;
    g_blit.n++;
    g_blit.dst = dst; g_blit.dx = dx; g_blit.dy = dy;
    g_blit.src = src; g_blit.sx = sx; g_blit.sy = sy; g_blit.w = w; g_blit.h = h;
    return 1;
}

static rsx_draw_backend g_stub_backend = {
    .user = NULL,
    .init = stub_init,
    .shutdown = stub_shutdown,
    .submit_and_wait = stub_submit,
    .texture_create = stub_texture_create,
    .texture_upload = stub_texture_upload,
    .texture_release = stub_texture_release,
    .color_target_create = stub_color_create,
    .color_target_release = stub_color_release,
    .color_target_write = stub_color_write,
    .surface_view = stub_surface_view,
    .depth_target_create = stub_depth_create,
    .depth_target_release = stub_release,
    .depth_snapshot = stub_depth_snapshot,
    .pipeline_create = stub_pipeline_create,
    .pipeline_release = stub_pipeline_release,
    .bind_targets = stub_bind_targets,
    .bind_pipeline = stub_bind_pipeline,
    .bind_vs_constants = stub_bind_vs,
    .bind_ps_constants = stub_bind_ps,
    .bind_textures = stub_bind_textures,
    .bind_vertex_textures = stub_bind_vtextures,
    .set_viewport = stub_viewport,
    .set_scissor = stub_scissor,
    .set_stencil_ref = stub_stencil_ref,
    .draw = stub_draw,
    .clear_color = stub_clear_color,
    .clear_depth_stencil = stub_clear_ds,
    .clear_color_rect = stub_clear_rect,
    .clear_depth_stencil_rect = stub_clear_ds_rect,
    .surface_region = stub_surface_region,
    .surface_copy = stub_surface_copy,
    .present = stub_present,
    .readback = stub_readback,
    .depth_readback = stub_depth_readback,
};

/* ---- driving the engine -------------------------------------------------- */

#define M_CONTEXT_DMA_COLOR_B 0x018C
#define M_CONTEXT_DMA_COLOR_A 0x0194
#define M_CONTEXT_DMA_COLOR_C 0x01B4
#define M_CONTEXT_DMA_COLOR_D 0x01B8
#define M_SURFACE_CLIP_H      0x0200
#define M_SURFACE_CLIP_V      0x0204
#define M_SURFACE_FORMAT      0x0208
#define M_COLOR_A_PITCH       0x020C
#define M_COLOR_A_OFFSET      0x0210
#define M_ZETA_OFFSET         0x0214
#define M_COLOR_B_OFFSET      0x0218
#define M_COLOR_TARGET        0x0220
#define M_CONTEXT_DMA_ZETA    0x0198
#define M_SHADER_WINDOW       0x1D88
#define M_ZSTENCIL_CLEAR      0x1D8C
#define M_ZETA_PITCH          0x022C
#define M_COLOR_MASK          0x0324
#define M_DEPTH_WRITE         0x0A70
#define M_DEPTH_TEST          0x0A74
#define M_DEPTH_FUNC          0x0A6C
#define M_TEX_SIZE1           0x1840
#define M_COLOR_C_OFFSET      0x0288
#define M_COLOR_D_OFFSET      0x028C
#define M_SCISSOR_H           0x08C0
#define M_SCISSOR_V           0x08C4
#define M_VTXBUF_OFFSET       0x1680
#define M_VTXFMT              0x1740
#define M_BEGIN_END           0x1808
#define M_DRAW_ARRAYS         0x1814
#define M_INLINE_ARRAY        0x1818
#define M_TEX_UNIT            0x1A00   /* + unit * 0x20 */
#define M_STENCIL_FUNC_REF    0x0334
#define M_CLEAR_BUFFERS       0x1D94
#define M_VP_UPLOAD_CONST_ID  0x1EFC
#define M_VP_UPLOAD_CONST     0x1F00

#define DMA_LOCAL  0xFEED0000u
#define DMA_MAIN   0xFEED0001u

#define VTX_OFFSET  0x00010000u        /* main memory, well clear of ea 0 */
#define TEX_OFFSET  0x00020000u

static void m(u32 method, u32 arg) { rsx_draw_engine_method(method, arg); }

static void engine_up(void)
{
    memset(&stub, 0, sizeof stub);
    rsx_draw_engine_set_backend(&g_stub_backend);
    if (rsx_draw_engine_init(1280, 720) != 0) {
        printf("[FAIL] engine init\n");
        exit(1);
    }
    /* One 256x256 surface in local memory, and a vertex array in main. */
    m(M_CONTEXT_DMA_COLOR_A, DMA_LOCAL);
    m(M_SURFACE_CLIP_H, 256u << 16);
    m(M_SURFACE_CLIP_V, 256u << 16);
    m(M_COLOR_A_OFFSET, 0x40000u);
    /* type 2 (float32), size 4, stride 16; bit 31 of the offset = main. */
    m(M_VTXFMT, 2u | (4u << 4) | (16u << 8));
    m(M_VTXBUF_OFFSET, 0x80000000u | VTX_OFFSET);
}

static void engine_down(void)
{
    rsx_draw_engine_shutdown();
    rsx_draw_engine_set_backend(NULL);
}

static void draw_triangle(void)
{
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    m(M_DRAW_ARRAYS, 0u | ((3u - 1u) << 24));
    m(M_BEGIN_END, 0u);
}

/* Bind unit 0 at `offset` in `location` (0 = local, 1 = main), 4x4 A8R8G8B8
 * linear with one level. */
static void bind_texture_unit(u32 location, u32 offset)
{
    const u32 dma = (location == RSX_LOCATION_MAIN) ? 2u : 1u;
    m(M_TEX_UNIT + 0x00, offset);
    m(M_TEX_UNIT + 0x04, dma | (2u << 4) | ((0x85u | 0x20u) << 8) | (1u << 16));
    m(M_TEX_UNIT + 0x0C, 0x80000000u);        /* control0: unit enable */
    m(M_TEX_UNIT + 0x10, 0xAAE4u);            /* identity crossbar */
    m(M_TEX_UNIT + 0x18, (4u << 16) | 4u);    /* image rect 4x4 */
}

/* ---- 1. the surface set -------------------------------------------------- */

static void test_surface_set(void)
{
    printf("-- surface set\n");
    engine_up();

    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == 1 && stub.n_clear_color == 1,
          "a clear registers the bound colour surface (%d create, %d clear)",
          stub.n_color_create, stub.n_clear_color);
    const u32 first = stub.last_created_color;

    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == 1,
          "the same (context, offset, size, format) is the same surface");

    /* A size change reallocates and retires the old target. */
    m(M_SURFACE_CLIP_H, 512u << 16);
    m(M_SURFACE_CLIP_V, 512u << 16);
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == 2 && stub.n_color_release == 1 &&
          stub.alive[first] == 0,
          "a size change reallocates and releases the old target");

    /* An offset change is a different surface, and does not disturb the
     * first. */
    m(M_COLOR_A_OFFSET, 0x80000u);
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == 3 && stub.n_color_release == 1,
          "a different offset is a different surface");

    /* The same offset in the OTHER context DMA is also a different surface:
     * RSX offsets are two overlapping number spaces. */
    m(M_CONTEXT_DMA_COLOR_A, DMA_MAIN);
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == 4 && stub.n_color_release == 1,
          "the same offset in another context DMA is another surface");

    /* Main memory that the IO table maps is seeded from the guest's own
     * bytes; local memory is not, because nothing guarantees it is backed. */
    CHECK(stub.n_color_seeded == 1,
          "only the main-memory surface was seeded from guest bytes (%d)",
          stub.n_color_seeded);

    /* An implausible clip does not destroy a live target. */
    const int creates = stub.n_color_create;
    m(M_SURFACE_CLIP_H, 60000u << 16);
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == creates,
          "an implausible surface declaration keeps the existing target");

    engine_down();
}

/* ---- 2. surface aliasing on texture bind --------------------------------- */

static void test_surface_aliasing(void)
{
    printf("-- surface aliasing\n");
    engine_up();

    /* Render into a surface in MAIN memory, then sample a texture unit bound
     * at the same offset in the same context: the unit must read the live
     * target, not the guest bytes behind it. That is what every reflection,
     * shadow composite and post-process pass depends on. */
    m(M_CONTEXT_DMA_COLOR_A, DMA_MAIN);
    m(M_COLOR_A_OFFSET, TEX_OFFSET);
    m(M_CLEAR_BUFFERS, 0xF0u);
    const u32 surface = stub.last_created_color;

    /* Now draw into a DIFFERENT surface while sampling that one. */
    m(M_COLOR_A_OFFSET, 0x90000u);
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    draw_triangle();
    CHECK(stub.n_draw == 1, "the draw executed");
    CHECK(stub.bound_mask == 1u, "unit 0 is the only bound unit (0x%X)",
          stub.bound_mask);
    CHECK(stub.bound_tex[0] == surface,
          "the unit samples the registered surface (%u, want %u)",
          stub.bound_tex[0], surface);
    CHECK(stub.n_texture_create == 0,
          "and nothing was uploaded from guest memory (%d uploads)",
          stub.n_texture_create);

    /* A unit naming the surface the pass is drawing INTO must not alias it:
     * reading a target a pass is writing is undefined on every API. */
    m(M_COLOR_A_OFFSET, TEX_OFFSET);
    draw_triangle();
    CHECK(stub.bound_tex[0] != surface,
          "a unit naming its own render target does not alias it");

    /* The same offset in the other context DMA is a different object. */
    m(M_COLOR_A_OFFSET, 0x90000u);
    bind_texture_unit(RSX_LOCATION_LOCAL, TEX_OFFSET);
    const int before = stub.n_texture_create;
    draw_triangle();
    CHECK(stub.n_texture_create == before + 1 && stub.bound_tex[0] != surface,
          "a unit in the other context uploads instead of aliasing");

    engine_down();
}

/* ---- 2b. the MRT set ----------------------------------------------------- */

static void test_mrt_set(void)
{
    printf("-- MRT set\n");
    engine_up();

    /* Targets B, C and D alongside the A engine_up declared, then MRT3 --
     * the way a deferred pass declares its G-buffer. */
    m(M_CONTEXT_DMA_COLOR_B, DMA_LOCAL);
    m(M_CONTEXT_DMA_COLOR_C, DMA_LOCAL);
    m(M_CONTEXT_DMA_COLOR_D, DMA_LOCAL);
    m(M_COLOR_B_OFFSET, 0x50000u);
    m(M_COLOR_C_OFFSET, 0x60000u);
    m(M_COLOR_D_OFFSET, 0x70000u);
    m(M_COLOR_TARGET, CELL_GCM_SURFACE_TARGET_MRT3);

    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_color_create == 4,
          "an MRT set registers all four targets (%d)", stub.n_color_create);
    CHECK(stub.n_clear_color == 4,
          "and one CLEAR_SURFACE clears every one of them (%d)",
          stub.n_clear_color);
    CHECK(stub.cleared[0] != stub.cleared[1] &&
          stub.cleared[1] != stub.cleared[2] &&
          stub.cleared[2] != stub.cleared[3] &&
          stub.cleared[0] != stub.cleared[3],
          "four distinct surfaces, not target A four times (%u,%u,%u,%u)",
          stub.cleared[0], stub.cleared[1], stub.cleared[2], stub.cleared[3]);

    draw_triangle();
    CHECK(stub.bound_rt_count == 4 && stub.bound_rt[0] == stub.cleared[0],
          "the draw binds all four, A first (%u bound)", stub.bound_rt_count);
    CHECK(stub.pipeline_rt_count == 4,
          "and its pipeline is built for four attachments (%u)",
          stub.pipeline_rt_count);

    /* A texture unit naming target C must not alias it: reading a target the
     * pass writes is undefined, and an MRT set writes more than one. */
    bind_texture_unit(RSX_LOCATION_LOCAL, 0x60000u);
    const int before = stub.n_texture_create;
    draw_triangle();
    CHECK(stub.n_texture_create == before + 1 &&
          stub.bound_tex[0] != stub.bound_rt[2],
          "a unit naming target C uploads instead of aliasing it");

    /* Back to target A alone: one attachment, and a pipeline of its own,
     * because the attachment count is part of the pipeline key. */
    const int pipelines = stub.n_pipeline_create;
    m(M_COLOR_TARGET, CELL_GCM_SURFACE_TARGET_0);
    draw_triangle();
    CHECK(stub.bound_rt_count == 1 && stub.pipeline_rt_count == 1 &&
          stub.n_pipeline_create == pipelines + 1,
          "target A alone binds one and builds its own pipeline (%u bound, %d)",
          stub.bound_rt_count, stub.n_pipeline_create);

    engine_down();
}

/* ---- 3. the texture cache ------------------------------------------------ */

static int raw_upload_result, raw_upload_calls;
static int stub_guest_upload(void* u, u32 t, u32 face, u32 mip, u32 w, u32 h,
                              const u8* src, const rsx_tex_layout* tl)
{
    (void)u; (void)t; (void)face; (void)mip;
    raw_upload_calls++;
    CHECK(w == 4 && h == 4 && tl->face_bytes == 64, "raw upload dimensions");
    CHECK(src[0] == 0x11 && src[63] == 0x11, "raw guest payload preserved");
    return raw_upload_result;
}

static void test_guest_texture_upload(void)
{
    for (int result = -1; result <= 1; result++) {
        g_stub_backend.texture_upload_guest = stub_guest_upload;
        engine_up();
        raw_upload_calls = 0;
        raw_upload_result = result;
        bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
        memset(vm_base + TEX_OFFSET, 0x11, 64);
        draw_triangle();
        CHECK(raw_upload_calls == 1, "raw callback invoked");
        CHECK(stub.n_texture_upload == (result == 0), "CPU fallback only when declined");
        if (result < 0)
            CHECK(stub.n_texture_release == 1, "failed raw upload releases resource");
        engine_down();
    }
    g_stub_backend.texture_upload_guest = NULL;
}

static void test_texture_cache(void)
{
    printf("-- texture cache\n");
    engine_up();
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    memset(vm_base + TEX_OFFSET, 0x11, 4 * 4 * 4);

    draw_triangle();
    CHECK(stub.n_texture_create == 1 && stub.n_texture_upload == 1,
          "the first bind uploads the texture");
    const u32 first = stub.last_created_texture;

    draw_triangle();
    draw_triangle();
    CHECK(stub.n_texture_create == 1,
          "a repeat bind in the same frame reuses it (%d uploads)",
          stub.n_texture_create);

    rsx_draw_engine_present();
    memset(vm_base + TEX_OFFSET, 0x22, 4 * 4 * 4);
    draw_triangle();
    CHECK(stub.n_texture_create == 1 && stub.n_texture_upload == 2 && stub.alive[first] == 1,
          "changed guest bytes are re-decoded into the same resource");

    rsx_draw_engine_present();
    draw_triangle();
    CHECK(stub.n_texture_create == 1 && stub.n_texture_upload == 2,
          "unchanged bytes in a later frame do not re-decode");

    /* Fill the cache past its capacity with distinct descriptors, then come
     * back to the first: it must have been evicted, not still resident, and
     * the most recent one must still be there. Returning nothing instead of
     * evicting is what made a recovered scene render as flat geometry. */
    for (u32 i = 0; i < RSX_DRAW_ENGINE_TEXTURE_CACHE; i++) {
        rsx_draw_engine_present();
        bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET + 0x1000u + i * 0x100u);
        draw_triangle();
    }
    CHECK(stub.n_texture_release >= 1,
          "the full cache evicts rather than giving up (%d evictions)",
          stub.n_texture_release);
    const u32 newest = stub.last_created_texture;
    const int creates = stub.n_texture_create;
    draw_triangle();
    CHECK(stub.n_texture_create == creates && stub.bound_tex[0] == newest,
          "the most recently used entry survived");

    rsx_draw_engine_present();
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET);
    draw_triangle();
    CHECK(stub.n_texture_create == creates + 1,
          "the least recently used entry was the one evicted");

    engine_down();
}

static void test_texture_out_of_memory(void)
{
    printf("-- texture out of device memory\n");
    engine_up();
    stub.texture_live_cap = 2;
    for (u32 i = 0; i < 3; i++) {
        rsx_draw_engine_present();
        bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET + 0x2000u + i * 0x100u);
        draw_triangle();
    }
    CHECK(stub.bound_tex[0] != 0 && stub.n_texture_release == 1 && stub.n_texture_live == 2,
          "a failed create evicts the least recently used texture and retries"
          " (%d releases, %d live)", stub.n_texture_release, stub.n_texture_live);
    const int creates = stub.n_texture_create;
    rsx_draw_engine_present();
    bind_texture_unit(RSX_LOCATION_MAIN, TEX_OFFSET + 0x2000u);
    draw_triangle();
    CHECK(stub.bound_tex[0] != 0 && stub.n_texture_create > creates,
          "the evicted texture is decoded again on its next use");
    engine_down();
}

/* ---- 4. the pipeline key ------------------------------------------------- */

static void test_pipeline_key(void)
{
    printf("-- pipeline key\n");

    /* The key over the render state directly: what must and must not change
     * it. The alpha REFERENCE lives in the fragment constant block, so a
     * title fading something out must not compile a pipeline per frame. */
    rsx_be_render_state a;
    memset(&a, 0, sizeof a);
    a.alpha_test_enable = 1;
    a.alpha_func = 0x0204;
    a.alpha_ref_raw = 0x40;
    a.depth_func = 0x0201;
    const u64 base = rsx_draw_engine_hash_render_state(&a, 1469598103934665603ull);

    rsx_be_render_state b = a;
    b.alpha_ref_raw = 0xC0;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) == base,
          "the alpha reference is not part of the key");

    b = a; b.alpha_func = 0x0202;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) != base,
          "the alpha compare mode is");

    b = a; b.depth_func = 0x0203;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) != base,
          "so is the depth compare mode");

    b = a; b.color_mask = 0x01010101u;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) != base,
          "so is the colour mask");

    b = a; b.rt_fp16 = 1;
    CHECK(rsx_draw_engine_hash_render_state(&b, 1469598103934665603ull) != base,
          "so is an FP16 render target");

    /* And through the engine: transform constants change every draw of every
     * frame, and must not multiply pipelines. */
    engine_up();
    draw_triangle();
    CHECK(stub.n_pipeline_create == 1, "the first draw builds a pipeline");

    for (u32 i = 0; i < 8; i++) {
        m(M_VP_UPLOAD_CONST_ID, i);
        for (u32 w = 0; w < 4; w++) m(M_VP_UPLOAD_CONST + w * 4, 0x3F800000u + i);
        draw_triangle();
    }
    CHECK(stub.n_pipeline_create == 1,
          "changing transform constants does not (%d pipelines)",
          stub.n_pipeline_create);

    /* The stencil reference is encoder state, not pipeline state. */
    m(M_STENCIL_FUNC_REF, 0x7Fu);
    draw_triangle();
    CHECK(stub.n_pipeline_create == 1 && stub.stencil_ref == 0x7Fu,
          "nor does the stencil reference, which reaches the encoder (%u)",
          stub.stencil_ref);

    engine_down();
}

static void test_pipeline_capacity(void)
{
    printf("-- pipeline capacity\n");
    engine_up();
    const u32 n = 600;
    for (u32 i = 0; i < n; i++) {
        m(M_DEPTH_FUNC, 0x0200u + i);
        draw_triangle();
    }
    CHECK(stub.n_pipeline_create == (int)n,
          "more than 512 distinct pipelines all reach the backend (%d of %u)",
          stub.n_pipeline_create, n);
    CHECK(stub.n_draw == (int)n, "and none of their draws is dropped (%d)", stub.n_draw);
    engine_down();
}

static void test_surface_subregions(void)
{
    printf("-- surface sub-regions\n");
    engine_up();
    m(M_COLOR_A_PITCH, 1024u);
    draw_triangle();
    const int created = stub.n_color_create;
    const u32 surface = stub.bound_surface;

    m(M_SURFACE_CLIP_H, 128u << 16);
    m(M_SURFACE_CLIP_V, 128u << 16);
    draw_triangle();
    CHECK(stub.n_color_create == created && stub.bound_surface == surface,
          "a smaller clip at the same address and pitch draws into the same target");
    CHECK(stub.scissor[2] == 128u && stub.scissor[3] == 128u,
          "with the scissor held to the clip (%ux%u)", stub.scissor[2], stub.scissor[3]);
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_clear_rect == 1 && stub.clear_rect[2] == 128u && stub.clear_rect[3] == 128u,
          "and a colour clear covers only the clip (%d, %ux%u)", stub.n_clear_rect,
          stub.clear_rect[2], stub.clear_rect[3]);

    m(M_SURFACE_CLIP_H, 256u << 16);
    m(M_SURFACE_CLIP_V, 256u << 16);
    m(M_SCISSOR_H, (64u << 16) | 16u);
    m(M_SCISSOR_V, (32u << 16) | 8u);
    const int full = stub.n_clear_color;
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_clear_rect == 2 && stub.n_clear_color == full &&
          stub.clear_rect[0] == 16u && stub.clear_rect[1] == 8u &&
          stub.clear_rect[2] == 64u && stub.clear_rect[3] == 32u,
          "a clear honours the guest scissor (%u,%u %ux%u)", stub.clear_rect[0],
          stub.clear_rect[1], stub.clear_rect[2], stub.clear_rect[3]);
    m(M_SCISSOR_H, 0u);
    m(M_SCISSOR_V, 0u);
    m(M_CLEAR_BUFFERS, 0xF0u);
    CHECK(stub.n_clear_color == full + 1, "and without one it clears the whole target");

    m(M_SURFACE_CLIP_H, 512u << 16);
    m(M_SURFACE_CLIP_V, 512u << 16);
    draw_triangle();
    const u32 grown = stub.bound_surface;
    CHECK(stub.n_color_create == created + 1 && stub.n_surface_copy == 1 &&
          stub.copy_size[0] == 256u && stub.copy_size[1] == 256u,
          "growing the clip keeps what was drawn (%d copies, %ux%u)", stub.n_surface_copy,
          stub.copy_size[0], stub.copy_size[1]);

    m(M_COLOR_A_OFFSET, 0x90000u);
    m(M_SURFACE_CLIP_H, 256u << 16);
    m(M_SURFACE_CLIP_V, 256u << 16);
    bind_texture_unit(RSX_LOCATION_LOCAL, 0x40000u);
    m(M_TEX_SIZE1, 1024u);
    draw_triangle();
    CHECK(stub.n_region == 1 && stub.region_size[0] == 4u && stub.region_size[1] == 4u &&
          stub.bound_tex[0] == (grown | 0x40000000u),
          "a smaller texture with the surface's pitch samples its top-left corner (%d, %ux%u)",
          stub.n_region, stub.region_size[0], stub.region_size[1]);
    engine_down();
}

/* ---- 5. restart cuts and topology expansion ------------------------------ */

static void test_restart_expansion(void)
{
    printf("-- restart cuts\n");

    /* Eight strip vertices with the guest's restart sentinel after the
     * fourth. Without the cut the expansion joins the two runs with two
     * triangles that were never in the mesh -- upstream's "exploded spiky
     * mesh that occluded the scene". */
    const u32 cuts[1] = { 4 };
    u32 idx[64];

    const u32 whole = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_TRIANGLE_STRIP, 8, NULL, 0);
    const u32 cut = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_TRIANGLE_STRIP, 8, cuts, 1);
    CHECK(whole == (8 - 2) * 3, "an uncut strip of 8 is 6 triangles (%u)", whole);
    CHECK(cut == (4 - 2) * 3 * 2,
          "a strip cut in half is two runs of 2 triangles (%u)", cut);

    memset(idx, 0xFF, sizeof idx);
    rsx_draw_engine_write_topology_indices(RSX_PRIMITIVE_TRIANGLE_STRIP, 8,
                                           cuts, 1, NULL, idx);
    int spans = 0;
    for (u32 t = 0; t < cut / 3; t++) {
        int below = 0, above = 0;
        for (u32 k = 0; k < 3; k++)
            if (idx[t * 3 + k] < 4) below++; else above++;
        if (below && above) spans = 1;
    }
    CHECK(!spans, "no triangle spans the cut");

    /* Winding alternates per triangle within a segment, and each segment
     * starts over: a strip's second triangle swaps its first two vertices. */
    CHECK(idx[0] == 0 && idx[1] == 1 && idx[2] == 2,
          "strip triangle 0 is 0,1,2 (%u,%u,%u)", idx[0], idx[1], idx[2]);
    CHECK(idx[3] == 2 && idx[4] == 1 && idx[5] == 3,
          "strip triangle 1 swaps to 2,1,3 (%u,%u,%u)", idx[3], idx[4], idx[5]);
    CHECK(idx[6] == 4 && idx[7] == 5 && idx[8] == 6,
          "the segment after the cut starts its winding again (%u,%u,%u)",
          idx[6], idx[7], idx[8]);

    /* Fans and quads are cut the same way. */
    const u32 fan = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_TRIANGLE_FAN, 8, cuts, 1);
    CHECK(fan == (4 - 2) * 3 * 2, "a cut fan is two runs of 2 triangles (%u)", fan);
    rsx_draw_engine_write_topology_indices(RSX_PRIMITIVE_TRIANGLE_FAN, 8,
                                           cuts, 1, NULL, idx);
    CHECK(idx[0] == 0 && idx[3] == 0 && idx[6] == 4 && idx[9] == 4,
          "each fan segment fans around its own first vertex");

    const u32 quads = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_QUADS, 8, NULL, 0);
    CHECK(quads == 12, "two quads are four triangles (%u)", quads);

    /* A quad strip advances two vertices per quad, so eight vertices are
     * three quads whole and one quad per half once the strip is cut. */
    const u32 qstrip = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_QUAD_STRIP, 8, NULL, 0);
    const u32 qstrip_cut = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_QUAD_STRIP, 8, cuts, 1);
    CHECK(qstrip == 3 * 6, "an uncut quad strip of 8 is three quads (%u)", qstrip);
    CHECK(qstrip_cut == 2 * 6,
          "a quad strip cut in half is one quad per half (%u)", qstrip_cut);

    memset(idx, 0xFF, sizeof idx);
    rsx_draw_engine_write_topology_indices(RSX_PRIMITIVE_QUAD_STRIP, 8,
                                           cuts, 1, NULL, idx);
    CHECK(idx[0] == 0 && idx[1] == 1 && idx[2] == 2 &&
          idx[3] == 1 && idx[4] == 3 && idx[5] == 2,
          "a quad is split along the diagonal between its pairs "
          "(%u,%u,%u %u,%u,%u)", idx[0], idx[1], idx[2], idx[3], idx[4], idx[5]);
    CHECK(idx[6] == 4 && idx[7] == 5 && idx[8] == 6,
          "the segment after the cut pairs from its own first vertex "
          "(%u,%u,%u)", idx[6], idx[7], idx[8]);
    spans = 0;
    for (u32 t = 0; t < qstrip_cut / 3; t++) {
        int below = 0, above = 0;
        for (u32 k = 0; k < 3; k++)
            if (idx[t * 3 + k] < 4) below++; else above++;
        if (below && above) spans = 1;
    }
    CHECK(!spans, "and no quad spans the cut");

    /* A polygon is a fan around its own first vertex, and a cut starts a
     * second polygon rather than joining the two. */
    const u32 poly = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_POLYGON, 5, NULL, 0);
    const u32 poly_cut = rsx_draw_engine_topology_index_count(
        RSX_PRIMITIVE_POLYGON, 8, cuts, 1);
    CHECK(poly == (5 - 2) * 3, "a pentagon is three triangles (%u)", poly);
    CHECK(poly_cut == (4 - 2) * 3 * 2,
          "a cut polygon is two runs of 2 triangles (%u)", poly_cut);

    memset(idx, 0xFF, sizeof idx);
    rsx_draw_engine_write_topology_indices(RSX_PRIMITIVE_POLYGON, 8,
                                           cuts, 1, NULL, idx);
    CHECK(idx[0] == 0 && idx[1] == 1 && idx[2] == 2 &&
          idx[3] == 0 && idx[4] == 2 && idx[5] == 3,
          "the first polygon fans around vertex 0 (%u,%u,%u %u,%u,%u)",
          idx[0], idx[1], idx[2], idx[3], idx[4], idx[5]);
    CHECK(idx[6] == 4 && idx[9] == 4,
          "the one after the cut fans around vertex 4 (%u, %u)", idx[6], idx[9]);

    /* An occurrence-to-unique map is applied to every emitted index, which is
     * what lets a remapped draw share one uploaded vertex per reference. */
    u32 remap[8];
    for (u32 i = 0; i < 8; i++) remap[i] = 7u - i;
    rsx_draw_engine_write_topology_indices(RSX_PRIMITIVE_TRIANGLES, 6,
                                           NULL, 0, remap, idx);
    CHECK(idx[0] == 7 && idx[5] == 2, "the remap is applied to every index");
}

/* ---- 6. what reaches the backend as an indexed draw ---------------------- */

static void test_indexed_draws(void)
{
    printf("-- indexed draws\n");
    engine_up();

    /* A triangle list of distinct vertices has nothing to share, so it is
     * drawn straight. */
    draw_triangle();
    CHECK(stub.draw_indices == 0 && stub.draw_vertices == 3,
          "a triangle list of distinct vertices is not indexed (%u verts, %u indices)",
          stub.draw_vertices, stub.draw_indices);

    /* A strip is expanded into a triangle list through an INDEX buffer: six
     * vertices become twelve indices over six uploaded vertices, rather than
     * twelve duplicated ones. */
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLE_STRIP);
    m(M_DRAW_ARRAYS, 0u | ((6u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_indices == (6 - 2) * 3 && stub.draw_vertices == 6,
          "a strip is indexed, not duplicated (%u verts, %u indices)",
          stub.draw_vertices, stub.draw_indices);

    /* Only the attributes the program reads are fetched, so the stride is the
     * layout's rather than all sixteen registers. The built-in program reads
     * every register, so this is the full one; a guest program that reads two
     * would halve it. */
    CHECK(stub.draw_stride == 16u * 16u,
          "the built-in program's layout is all sixteen attributes (%u bytes)",
          stub.draw_stride);

    /* Quads have no host equivalent and become an indexed triangle list. */
    m(M_BEGIN_END, RSX_PRIMITIVE_QUADS);
    m(M_DRAW_ARRAYS, 0u | ((8u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_TRIANGLES &&
          stub.draw_indices == 12,
          "two quads become twelve triangle indices (%u)", stub.draw_indices);

    /* So do quad strips and polygons, the two the engine used to drop. Six
     * quad-strip vertices are two quads, and a five-vertex polygon is a fan
     * of three triangles. */
    m(M_BEGIN_END, RSX_PRIMITIVE_QUAD_STRIP);
    m(M_DRAW_ARRAYS, 0u | ((6u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_TRIANGLES &&
          stub.draw_indices == 12 && stub.draw_vertices == 6,
          "a quad strip of six becomes twelve indices over six vertices "
          "(%u verts, %u indices)", stub.draw_vertices, stub.draw_indices);

    m(M_BEGIN_END, RSX_PRIMITIVE_POLYGON);
    m(M_DRAW_ARRAYS, 0u | ((5u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_TRIANGLES &&
          stub.draw_indices == 9 && stub.draw_vertices == 5,
          "a five-vertex polygon becomes nine indices (%u verts, %u indices)",
          stub.draw_vertices, stub.draw_indices);

    /* Points and lines are drawn as they are, in the order they arrived: an
     * index buffer would be pointless and collapsing repeats would reorder
     * them. */
    m(M_BEGIN_END, RSX_PRIMITIVE_LINES);
    m(M_DRAW_ARRAYS, 0u | ((4u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_LINES && stub.draw_indices == 0 &&
          stub.draw_vertices == 4,
          "lines pass through unindexed (%u verts, %u indices)",
          stub.draw_vertices, stub.draw_indices);

    m(M_BEGIN_END, RSX_PRIMITIVE_POINTS);
    m(M_DRAW_ARRAYS, 0u | ((2u - 1u) << 24));
    m(M_BEGIN_END, 0u);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_POINTS && stub.draw_vertices == 2,
          "so do points (%u verts)", stub.draw_vertices);

    engine_down();
}

/* ---- 7. the guest scissor ------------------------------------------------ */

static void test_scissor(void)
{
    printf("-- scissor\n");
    engine_up();

    draw_triangle();
    CHECK(stub.scissor[2] == 256 && stub.scissor[3] == 256,
          "an unwritten scissor register is the whole surface (%ux%u)",
          stub.scissor[2], stub.scissor[3]);

    m(M_SCISSOR_H, (64u << 16) | 16u);
    m(M_SCISSOR_V, (32u << 16) | 8u);
    draw_triangle();
    CHECK(stub.scissor[0] == 16 && stub.scissor[1] == 8 &&
          stub.scissor[2] == 64 && stub.scissor[3] == 32,
          "a guest scissor narrows it (%u,%u %ux%u)",
          stub.scissor[0], stub.scissor[1], stub.scissor[2], stub.scissor[3]);

    m(M_SCISSOR_H, (4096u << 16) | 0u);
    m(M_SCISSOR_V, (4096u << 16) | 0u);
    draw_triangle();
    CHECK(stub.scissor[2] == 256 && stub.scissor[3] == 256,
          "a scissor larger than the surface is clipped to it (%ux%u)",
          stub.scissor[2], stub.scissor[3]);

    engine_down();
}

static void test_shader_window(void)
{
    printf("-- shader window\n");
    engine_up();

    draw_triangle();
    CHECK(stub.ps_tail[1] == 1.0f && stub.ps_tail[2] == 0.0f && stub.ps_tail[3] == 0.0f,
          "an unwritten SHADER_WINDOW is the reset value 0x1000, origin bottom (%g %g %g)",
          stub.ps_tail[1], stub.ps_tail[2], stub.ps_tail[3]);

    m(M_SHADER_WINDOW, 0x1000u | 256u);
    draw_triangle();
    CHECK(stub.ps_tail[1] == 1.0f && stub.ps_tail[2] == 256.0f && stub.ps_tail[3] == 0.0f,
          "origin bottom reaches the fragment constants with the window height (%g %g %g)",
          stub.ps_tail[1], stub.ps_tail[2], stub.ps_tail[3]);

    m(M_SHADER_WINDOW, 0x10000u | 256u);
    draw_triangle();
    CHECK(stub.ps_tail[1] == 0.0f && stub.ps_tail[2] == 256.0f && stub.ps_tail[3] == -0.5f,
          "origin top with integer centres keeps y and moves the centre (%g %g %g)",
          stub.ps_tail[1], stub.ps_tail[2], stub.ps_tail[3]);

    engine_down();
}

/* ---- 8. the presented pixel ---------------------------------------------- */

static void test_readback(void)
{
    printf("-- readback\n");
    engine_up();
    m(M_CLEAR_BUFFERS, 0xF0u);
    rsx_draw_engine_present();
    CHECK(stub.n_present == 1, "the flip presented a surface");
    CHECK(rsx_draw_engine_readback_center() == 0x44112233u,
          "the presented pixel comes back as A8R8G8B8 (0x%08X)",
          rsx_draw_engine_readback_center());
    engine_down();
}

static void test_queued_flip_buffer(void)
{
    engine_up();
    draw_triangle();
    u32 first = stub.bound_rt[0];
    rsx_draw_engine_set_display_buffer(0, RSX_LOCATION_LOCAL, 0x40000, 1024, 256, 256);
    m(M_COLOR_A_OFFSET, 0x60000);
    draw_triangle();
    u32 second = stub.bound_rt[0];
    rsx_draw_engine_set_display_buffer(1, RSX_LOCATION_LOCAL, 0x60000, 1024, 256, 256);
    rsx_draw_engine_present_buffer(0);
    CHECK(presented_surface == first, "queued flip selects first buffer over current target");
    rsx_draw_engine_present_buffer(1);
    CHECK(presented_surface == second && first != second, "queued flip selects second buffer");
    rsx_draw_engine_present();
    CHECK(presented_surface == second, "host present retains last queued buffer");
    int presents = stub.n_present;
    rsx_draw_engine_note_flip(0);
    CHECK(stub.n_present == presents, "noting a flip does not present");
    rsx_draw_engine_present();
    CHECK(presented_surface == first, "host present follows the noted flip");
    engine_down();
}

/* A 2D transfer reads and writes guest memory; the pixels it touches live in
 * the engine's surfaces, so they are synced both ways around it. */
static void test_transfer_coherence(void)
{
    printf("-- transfer coherence\n");
    engine_up();
    m(M_COLOR_A_PITCH, 1024u);                 /* 256 pixels of A8R8G8B8 */
    draw_triangle();
    const u32 surface = stub.bound_rt[0];

    /* The guest wrote A,R,G,B bytes at (2,3): they reach the surface as R,G,B,A. */
    u8* px = vm_base + GUEST_LOCAL_EA + 0x40000u + 3u * 1024u + 2u * 4u;
    px[0] = 0x80; px[1] = 0x01; px[2] = 0x02; px[3] = 0x03;
    memset(&g_written, 0, sizeof g_written);
    rsx_draw_engine_guest_wrote(RSX_LOCATION_LOCAL, 0x40000u + 3u * 1024u + 2u * 4u,
                                1024u, 5u, 4u);
    CHECK(g_written.n == 1 && g_written.surface == surface,
          "a transfer into a surface's bytes is pushed to that surface");
    CHECK(g_written.x == 2 && g_written.y == 3 && g_written.w == 5 && g_written.h == 4,
          "at the rectangle the bytes describe (%u,%u %ux%u)",
          g_written.x, g_written.y, g_written.w, g_written.h);
    CHECK(g_written.first[0] == 0x01 && g_written.first[1] == 0x02 &&
          g_written.first[2] == 0x03 && g_written.first[3] == 0x80,
          "A8R8G8B8 guest bytes arrive as R,G,B,A");

    /* Another pitch lays the rows out elsewhere, and other memory is not a surface. */
    rsx_draw_engine_guest_wrote(RSX_LOCATION_LOCAL, 0x40000u, 2048u, 4u, 4u);
    rsx_draw_engine_guest_wrote(RSX_LOCATION_MAIN, 0x40000u, 1024u, 4u, 4u);
    CHECK(g_written.n == 1, "a different pitch or memory space touches no surface");

    /* A transfer reading a surface's bytes gets the GPU's pixels first. */
    u8* rd = vm_base + GUEST_LOCAL_EA + 0x40000u + 1024u;
    memset(rd, 0, 8);
    rsx_draw_engine_guest_read(RSX_LOCATION_LOCAL, 0x40000u + 1024u, 1024u, 2u, 1u);
    CHECK(rd[0] == 0x44 && rd[1] == 0x11 && rd[2] == 0x22 && rd[3] == 0x33 &&
          rd[4] == 0x44 && rd[7] == 0x33,
          "the surface's R,G,B,A pixels land in guest memory as A8R8G8B8");
    engine_down();
}

static unsigned custom_reads;
static const u8* custom_guest_read(void* user, u32 location, u32 offset, u32 bytes)
{
    CHECK(user == &custom_reads, "custom memory resolver receives its user data");
    ++custom_reads;
    u32 ea = location == 0 ? GUEST_LOCAL_EA + offset : offset;
    if ((u64)ea + bytes > GUEST_BYTES) return NULL;
    return vm_base + ea;
}

static void test_custom_guest_mapping(void)
{
    engine_up();
    rsx_draw_engine_set_guest_memory(custom_guest_read, &custom_reads);
    custom_reads = 0;
    draw_triangle();
    CHECK(custom_reads > 0, "draw resolves vertices through runner's memory map");
    unsigned calls = custom_reads;
    rsx_draw_engine_set_guest_memory(NULL, NULL);
    draw_triangle();
    CHECK(custom_reads == calls, "clearing resolver restores default mapping");
    engine_down();
}

/* NV4097_INLINE_ARRAY: the vertices arrive IN the pushbuffer.
 *
 * Nothing consumed method 0x1818 at all, so a title that builds geometry this
 * way (Guitar Hero III draws its whole 2D layer as inline QUADS) issued draws
 * that reached the backend with no vertices and were counted as empty groups.
 * The regression this guards is silent by construction -- the draw is simply
 * absent -- so the check is both that a draw happens AND that its vertices
 * came from the stream rather than from the still-bound vertex array. */
static u32 f32_bits(float f) { u32 u; memcpy(&u, &f, 4); return u; }

static void test_inline_array(void)
{
    engine_up();
    /* ATTR0 alone: float4, stride 16. The array at VTX_OFFSET holds 1.0f in
     * every component (see main), so reading the array instead of the stream
     * is a distinguishable wrong answer. */
    m(M_VTXFMT, 2u | (4u << 4) | (16u << 8));

    const int before = stub.n_draw;
    m(M_BEGIN_END, RSX_PRIMITIVE_TRIANGLES);
    for (u32 v = 0; v < 3; v++) {
        m(M_INLINE_ARRAY, f32_bits(10.0f + (float)v));  /* x: 10, 11, 12 */
        m(M_INLINE_ARRAY, f32_bits(20.0f));
        m(M_INLINE_ARRAY, f32_bits(30.0f));
        m(M_INLINE_ARRAY, f32_bits(1.0f));
    }
    m(M_BEGIN_END, 0u);

    CHECK(stub.n_draw == before + 1, "inline array issues a draw");
    CHECK(stub.draw_vertices == 3, "inline array draws 3 vertices (got %u)",
          stub.draw_vertices);
    CHECK(stub.draw_topology == RSX_TOPOLOGY_TRIANGLES,
          "inline array keeps the primitive type");
    CHECK(stub.draw_first_vertex[0] == 10.0f &&
          stub.draw_first_vertex[1] == 20.0f &&
          stub.draw_first_vertex[2] == 30.0f,
          "inline vertices come from the stream, not the vertex array "
          "(got %g,%g,%g)", stub.draw_first_vertex[0],
          stub.draw_first_vertex[1], stub.draw_first_vertex[2]);
    engine_down();
}

/* ---- 9. the depth clear value --------------------------------------------- */

#define ZETA_OFFSET  0x00100000u        /* local memory */

static void test_depth_clear_value(void)
{
    printf("-- depth clear value\n");
    engine_up();
    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_ZETA_OFFSET, ZETA_OFFSET);

    /* Z16 (depth field 1): the clear value is a bare depth16, and 0xFFFF its
     * far plane. Read as Z24S8 it would clear to 255/16777215. */
    m(M_SURFACE_FORMAT, 8u | (1u << 5));
    m(M_ZSTENCIL_CLEAR, 0xFFFFu);
    m(M_CLEAR_BUFFERS, 0x01u);
    CHECK(stub.n_clear_ds + stub.n_clear_ds_rect == 1 && stub.last_clear_depth == 1.0f,
          "a Z16 zeta cleared with 0xFFFF clears to 1.0 (%g)", stub.last_clear_depth);

    /* Z24S8 (depth field 2): depth24 << 8 | stencil8. */
    m(M_SURFACE_FORMAT, 8u | (2u << 5));
    m(M_ZSTENCIL_CLEAR, 0xFFFFFF00u);
    m(M_CLEAR_BUFFERS, 0x01u);
    CHECK(stub.last_clear_depth == 1.0f,
          "a Z24S8 zeta cleared with 0xFFFFFF00 clears to 1.0 (%g)", stub.last_clear_depth);

    m(0x03B0u, 1u << 12);
    m(M_ZSTENCIL_CLEAR, 0xBB86E600u);
    m(M_CLEAR_BUFFERS, 0x01u);
    CHECK(stub.last_clear_depth == 1.0f,
          "a float Z24S8 zeta cleared past the far plane clears to 1.0 (%g)", stub.last_clear_depth);
    m(M_ZSTENCIL_CLEAR, 0x7E000000u);
    m(M_CLEAR_BUFFERS, 0x01u);
    CHECK(stub.last_clear_depth == 0.5f,
          "a float Z24S8 clear value is an f32 shifted right by 7 (%g)", stub.last_clear_depth);
    m(0x03B0u, 0u);
    engine_down();
}

/* ---- 10. a zeta or a target read in a texture format of its own --------- */

static void test_depth_read_as_color(void)
{
    printf("-- zeta read as A8R8G8B8\n");
    engine_up();
    /* A depth-only pass, with colour A named at the zeta's own address and
     * nothing written to it: how a title points an unused colour target. */
    m(M_CONTEXT_DMA_ZETA, DMA_LOCAL);
    m(M_SURFACE_FORMAT, 8u | (2u << 5));
    m(M_ZETA_OFFSET, ZETA_OFFSET);
    m(M_ZETA_PITCH, 256u * 4u);
    m(M_COLOR_A_OFFSET, ZETA_OFFSET);
    m(M_COLOR_MASK, 0u);
    m(M_DEPTH_TEST, 1u);
    m(M_DEPTH_WRITE, 1u);
    draw_triangle();
    CHECK(stub.n_draw == 1, "the depth-only pass drew");

    /* The next pass reads that address as A8R8G8B8, the zeta still bound:
     * the bytes are the zeta's, stored to guest memory from the depth
     * target, not the untouched colour target's. */
    m(M_COLOR_MASK, 0x01010101u);
    m(M_DEPTH_TEST, 0u);
    m(M_DEPTH_WRITE, 0u);
    m(M_COLOR_A_OFFSET, 0x90000u);
    bind_texture_unit(RSX_LOCATION_LOCAL, ZETA_OFFSET);
    const int uploads = stub.n_texture_create;
    draw_triangle();
    const u8* z = vm_base + GUEST_LOCAL_EA + ZETA_OFFSET;
    CHECK(stub.n_depth_readback == 1, "the depth target was read back (%d)",
          stub.n_depth_readback);
    CHECK(z[0] == 0x80 && z[1] == 0x00 && z[2] == 0x00 && z[3] == 0x00,
          "guest memory holds depth 0.5 laid out as Z24S8 (%02X %02X %02X %02X)",
          z[0], z[1], z[2], z[3]);
    CHECK(stub.n_texture_create == uploads + 1 &&
          stub.bound_tex[0] == stub.last_created_texture,
          "the unit samples a texture decoded from those bytes");

    /* No depth write since: nothing is read back again. */
    draw_triangle();
    CHECK(stub.n_depth_readback == 1,
          "an unchanged depth target is not read back again (%d)", stub.n_depth_readback);
    engine_down();
}

static void test_color_read_as_float(void)
{
    printf("-- colour target read as X32_FLOAT\n");
    engine_up();
    m(M_COLOR_A_PITCH, 256u * 4u);
    m(M_CLEAR_BUFFERS, 0xF0u);                   /* a colour write at 0x40000 */
    const u32 surface = stub.last_created_color;

    /* Read back as X32_FLOAT, the target's 32-bit pixels are floats a pass
     * packed into it: they come from guest memory, where the target's pixels
     * are stored as A,R,G,B first. */
    m(M_COLOR_A_OFFSET, 0x90000u);
    bind_texture_unit(RSX_LOCATION_LOCAL, 0x40000u);
    m(M_TEX_UNIT + 0x04, 1u | (2u << 4) | ((0x9Cu | 0x20u) << 8) | (1u << 16));
    const int uploads = stub.n_texture_create;
    draw_triangle();
    const u8* p = vm_base + GUEST_LOCAL_EA + 0x40000u;
    CHECK(p[0] == 0x44 && p[1] == 0x11 && p[2] == 0x22 && p[3] == 0x33,
          "the target was stored to guest memory as A,R,G,B (%02X %02X %02X %02X)",
          p[0], p[1], p[2], p[3]);
    CHECK(stub.n_texture_create == uploads + 1 && stub.bound_tex[0] != surface,
          "the unit decodes those bytes instead of sampling the RGBA8 image");
    engine_down();
}

static void test_targets_read_on_the_gpu(void)
{
    printf("-- targets reinterpreted and blitted on the GPU\n");
    g_stub_backend.texture_from_target = stub_texture_from_target;
    g_stub_backend.surface_blit = stub_surface_blit;
    memset(&g_from_target, 0, sizeof g_from_target);
    memset(&g_blit, 0, sizeof g_blit);
    engine_up();
    m(M_COLOR_A_PITCH, 256u * 4u);
    m(M_CLEAR_BUFFERS, 0xF0u);
    const u32 surface = stub.last_created_color;

    u8* p = vm_base + GUEST_LOCAL_EA + 0x40000u;
    memset(p, 0, 16);
    m(M_COLOR_A_OFFSET, 0x90000u);
    bind_texture_unit(RSX_LOCATION_LOCAL, 0x40000u);
    m(M_TEX_UNIT + 0x04, 1u | (2u << 4) | ((0x9Cu | 0x20u) << 8) | (1u << 16));
    m(0x1840u, 256u * 4u);
    const int readbacks = g_readbacks;
    draw_triangle();
    CHECK(g_from_target.n == 1 && g_from_target.target == surface &&
          g_from_target.depth_bytes == 0 && g_from_target.pitch == 1024u &&
          g_from_target.w == 4 && g_from_target.h == 4 && g_from_target.base_format == 0x9Cu,
          "an RGBA8 target read as X32_FLOAT is converted from the target on the GPU");
    CHECK(stub.bound_tex[0] == g_from_target.texture && stub.bound_tex[0] != surface,
          "the unit samples the converted texture");
    CHECK(g_readbacks == readbacks && p[0] == 0 && p[1] == 0,
          "nothing is read back and guest memory is left alone");
    draw_triangle();
    CHECK(g_from_target.n == 1, "an unchanged target is not converted again (%d)",
          g_from_target.n);
    m(M_COLOR_A_OFFSET, 0x40000u);
    m(M_CLEAR_BUFFERS, 0xF0u);
    m(M_COLOR_A_OFFSET, 0x90000u);
    draw_triangle();
    CHECK(g_from_target.n == 2 && stub.bound_tex[0] == g_from_target.texture,
          "a write to the target converts it again into the same texture (%d)",
          g_from_target.n);

    m(M_COLOR_A_OFFSET, 0x40000u);
    draw_triangle();
    m(M_COLOR_A_OFFSET, 0xC0000u);
    draw_triangle();
    const u32 dst = stub.bound_rt[0];
    CHECK(rsx_draw_engine_blit(RSX_LOCATION_LOCAL, 0x40000u + 2u * 1024u + 8u, 1024u,
                               RSX_LOCATION_LOCAL, 0xC0000u + 5u * 1024u, 1024u, 16u, 8u) == 1 &&
          g_blit.n == 1 && g_blit.src == surface && g_blit.dst == dst &&
          g_blit.sx == 2 && g_blit.sy == 2 && g_blit.dx == 0 && g_blit.dy == 5 &&
          g_blit.w == 16 && g_blit.h == 8,
          "a transfer between two targets is a GPU copy at the rectangles' positions");
    CHECK(rsx_draw_engine_blit(RSX_LOCATION_LOCAL, 0x40000u, 1024u,
                               RSX_LOCATION_MAIN, 0x200000u, 1024u, 16u, 8u) == 0 &&
          rsx_draw_engine_blit(RSX_LOCATION_LOCAL, 0x40000u + 250u * 4u, 1024u,
                               RSX_LOCATION_LOCAL, 0xC0000u, 1024u, 16u, 8u) == 0 &&
          g_blit.n == 1,
          "a destination that is no target, or a rectangle past the edge, stays on the CPU path");

    const int before = g_readbacks;
    rsx_draw_engine_guest_read(RSX_LOCATION_LOCAL, 0x40000u, 1024u, 200u, 256u);
    rsx_draw_engine_guest_read(RSX_LOCATION_LOCAL, 0x40000u + 200u * 4u, 1024u, 56u, 256u);
    CHECK(g_readbacks == before + 1 && p[1024u * 255u + 255u * 4u] == 0x44,
          "a read of every row stores the whole target once, and the next read reuses it (%d)",
          g_readbacks - before);
    engine_down();
    g_stub_backend.texture_from_target = NULL;
    g_stub_backend.surface_blit = NULL;
}

#define VC_VTX_OFFSET 0x00200000u
#define VC_IDX_OFFSET 0x00300000u
#define VC_VERTS      3000u
#define VC_INDICES    1500u
#define VC_RESTART    97u

static void vc_put16(u8* p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

static void vc_fill(void)
{
    u64 rng = 0x2545F4914F6CDD1Dull;
    for (u32 i = 0; i < VC_VERTS * 4u; i++) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        const float f = (float)(rng % 2001u) / 1000.0f - 1.0f;
        u32 w;
        memcpy(&w, &f, 4);
        u8* p = vm_base + VC_VTX_OFFSET + i * 4u;
        p[0] = (u8)(w >> 24); p[1] = (u8)(w >> 16); p[2] = (u8)(w >> 8); p[3] = (u8)w;
    }
    for (u32 i = 0; i < VC_INDICES; i++) {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        vc_put16(vm_base + VC_IDX_OFFSET + i * 2u, (i % VC_RESTART == VC_RESTART - 1u)
                 ? 0xFFFFu : (u32)(rng % (VC_VERTS - 10u)));
    }
}

static u32 vc_index(u32 i)
{
    const u8* p = vm_base + VC_IDX_OFFSET + i * 2u;
    return ((u32)p[0] << 8) | p[1];
}

static void vc_mutate_vertex(void) { vm_base[VC_VTX_OFFSET + vc_index(5) * 16u + 6u] ^= 0x5Au; }
static void vc_mutate_index(void) { vc_put16(vm_base + VC_IDX_OFFSET + 7u * 2u, (vc_index(7) + 1u) % (VC_VERTS - 10u)); }
static void vc_mutate_unused(void) { vm_base[VC_VTX_OFFSET + (VC_VERTS - 1u) * 16u] ^= 0x33u; }

static void vc_engine_up(const char* cache, const char* threads)
{
    setenv("RSX_ENGINE_VERTEX_CACHE", cache, 1);
    setenv("RSX_ENGINE_VERTEX_THREADS", threads, 1);
    setenv("RSX_ENGINE_VERTEX_THREADS_MIN", "1", 1);
    engine_up();
}

static u64 vc_draw(u32 prim)
{
    m(0x181C, VC_IDX_OFFSET);
    m(0x1820, 1u | (1u << 4));
    m(0x1DAC, 1u);
    m(0x1DB0, 0xFFFFu);
    m(M_VTXBUF_OFFSET, 0x80000000u | VC_VTX_OFFSET);
    m(M_BEGIN_END, prim);
    for (u32 first = 0; first < VC_INDICES; first += 256u) {
        const u32 n = VC_INDICES - first < 256u ? VC_INDICES - first : 256u;
        m(0x1824, first | ((n - 1u) << 24));
    }
    m(M_BEGIN_END, 0u);
    return g_draw_hash;
}

static u64 vc_reference(u32 prim)
{
    vc_engine_up("0", "0");
    const u64 h = vc_draw(prim);
    engine_down();
    return h;
}

static void test_vertex_cache_prim(u32 prim, const char* name)
{
    vc_fill();
    const u64 base = vc_reference(prim);
    vc_mutate_vertex();
    const u64 after_vertex = vc_reference(prim);
    vc_mutate_index();
    const u64 after_index = vc_reference(prim);
    CHECK(base != after_vertex && after_vertex != after_index,
          "%s: the mutations change the draw", name);

    vc_fill();
    vc_engine_up("0", "4");
    CHECK(vc_draw(prim) == base, "%s: worker threads give the serial bytes", name);
    engine_down();

    vc_engine_up("1", "4");
    int same = 1;
    for (int i = 0; i < 4; i++) same &= vc_draw(prim) == base;
    rsx_vcache_stats st;
    rsx_draw_engine_vertex_cache_stats(&st);
    CHECK(same, "%s: repeated cached draws give the uncached bytes", name);
    CHECK(st.stores == 1 && st.hits == 2,
          "%s: stored on the second sighting, hit after (stores %llu, hits %llu)", name,
          (unsigned long long)st.stores, (unsigned long long)st.hits);
    vc_mutate_unused();
    CHECK(vc_draw(prim) == base, "%s: a byte outside the drawn span still hits", name);
    rsx_draw_engine_vertex_cache_stats(&st);
    CHECK(st.hits == 3, "%s: that draw was a hit (%llu)", name, (unsigned long long)st.hits);
    vc_mutate_unused();
    vc_mutate_vertex();
    CHECK(vc_draw(prim) == after_vertex, "%s: a vertex rewritten in place is seen", name);
    CHECK(vc_draw(prim) == after_vertex, "%s: and cached again", name);
    vc_mutate_index();
    CHECK(vc_draw(prim) == after_index, "%s: an index rewritten in place is seen", name);
    for (int i = 0; i < 3; i++) same &= vc_draw(prim) == after_index;
    CHECK(same, "%s: later draws keep the new bytes", name);
    rsx_draw_engine_vertex_cache_stats(&st);
    CHECK(st.misses_changed >= 2, "%s: the rewrites were changed-content misses (%llu)", name,
          (unsigned long long)st.misses_changed);
    engine_down();
}

static void test_vertex_cache(void)
{
    printf("-- vertex cache and worker threads\n");
    test_vertex_cache_prim(RSX_PRIMITIVE_TRIANGLE_STRIP, "strip");
    test_vertex_cache_prim(RSX_PRIMITIVE_TRIANGLES, "list");
    test_vertex_cache_prim(RSX_PRIMITIVE_QUADS, "quads");
    test_vertex_cache_prim(RSX_PRIMITIVE_LINES, "lines");
    unsetenv("RSX_ENGINE_VERTEX_CACHE");
    unsetenv("RSX_ENGINE_VERTEX_THREADS");
    unsetenv("RSX_ENGINE_VERTEX_THREADS_MIN");
}

int main(void)
{
    vm_base = (u8*)calloc(1, GUEST_BYTES);
    if (!vm_base) { printf("[FAIL] guest memory\n"); return 1; }
    ppu_vm_size = GUEST_BYTES;
    /* Three vertices of float4 position, so the fetch has something real to
     * read: the engine treats an unreadable ATTR0 as a failed group. */
    for (u32 i = 0; i < 3 * 4; i++) {
        u8* p = vm_base + VTX_OFFSET + i * 4;
        p[0] = 0x3F; p[1] = 0x80; p[2] = 0; p[3] = 0;    /* 1.0f big-endian */
    }

    test_surface_set();
    test_surface_aliasing();
    test_mrt_set();
    test_texture_cache();
    test_guest_texture_upload();
    test_texture_out_of_memory();
    test_pipeline_key();
    test_pipeline_capacity();
    test_surface_subregions();
    test_restart_expansion();
    test_indexed_draws();
    test_scissor();
    test_shader_window();
    test_readback();
    test_custom_guest_mapping();
    test_queued_flip_buffer();
    test_transfer_coherence();
    test_inline_array();
    test_depth_clear_value();
    test_depth_read_as_color();
    test_color_read_as_float();
    test_targets_read_on_the_gpu();
    test_vertex_cache();

    free(vm_base);
    printf(g_failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n",
           g_failures);
    return g_failures ? 1 : 0;
}
