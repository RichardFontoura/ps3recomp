#include "../rsx_vulkan_backend.c"

typedef struct pm_vertex {
    float pos[4];
    float col[4];
} pm_vertex;

typedef struct pm_counts {
    u64 opened, merged;
} pm_counts;

static const char* const kPmVS =
"struct VSInput { float4 a0:ATTR0; float4 a1:ATTR1; };\n"
"struct VSOutput { float4 pos:SV_Position; float4 col0:COLOR0; float4 t0:TEXCOORD0; };\n"
"VSOutput main(VSInput input) {\n"
"    VSOutput o; o.pos = input.a0; o.col0 = input.a1; o.t0 = input.a1; return o;\n"
"}\n";

static const char* const kPmSolidPS =
"struct PSInput { float4 position:SV_POSITION; float4 col0:COLOR0; float4 t0:TEXCOORD0; };\n"
"float4 main(PSInput input) : SV_TARGET { return input.col0; }\n";

static const char* const kPmTexturePS =
"Texture2D    rsx_tex[16] : register(t0);\n"
"SamplerState rsx_samp[16] : register(s0);\n"
"struct PSInput { float4 position:SV_POSITION; float4 col0:COLOR0; float4 t0:TEXCOORD0; };\n"
"float4 main(PSInput input) : SV_TARGET { return rsx_tex[0].Sample(rsx_samp[0], input.t0.xy); }\n";

static const float kRed[4]    = { 1, 0, 0, 1 };
static const float kGreen[4]  = { 0, 1, 0, 1 };
static const float kBlue[4]   = { 0, 0, 1, 1 };
static const float kYellow[4] = { 1, 1, 0, 1 };
static const float kCyan[4]   = { 0, 1, 1, 1 };

static const rsx_draw_backend* be = &s_vk_engine_backend;
static int checks, failures;
static u32 solid, solid_depth, textured;

volatile int g_ps3_timeline_on;
u8* vm_base;
u32 ppu_vm_size;

void ps3_timeline_event(uint32_t t, uint32_t a) { (void)t; (void)a; }
u32 cellGcmResolveLocated(int local, u32 offset) { (void)local; return offset; }
u32 cellGcmResolveIO(u32 offset) { return offset; }
u32 cellGcm_flip_request_count(void) { return 0; }
void cellGcm_replay_display_buffers_to_engine(void) {}
void rsx_set_backend(rsx_backend* backend) { (void)backend; }
rsx_backend* rsx_get_backend(void) { return NULL; }

static void check(int ok, const char* what)
{
    checks++;
    if (!ok) { failures++; fprintf(stderr, "FAIL: %s\n", what); }
}

static pm_counts counts(void)
{
    pm_counts c = { s_pass.opened, s_pass.merged };
    return c;
}

static u32 pixel(u32 target, u32 x, u32 y)
{
    u8 px[4] = { 0 };
    be->readback(NULL, target, x, y, 1, 1, px, 4);
    return ((u32)px[0] << 24) | ((u32)px[1] << 16) | ((u32)px[2] << 8) | px[3];
}

static u32 rgba(const float c[4])
{
    return ((u32)(c[0] * 255.0f) << 24) | ((u32)(c[1] * 255.0f) << 16) |
           ((u32)(c[2] * 255.0f) << 8) | (u32)(c[3] * 255.0f);
}

static u32 make_pipeline(const char* ps, int depth)
{
    rsx_be_render_state rs;
    memset(&rs, 0, sizeof rs);
    rs.color_mask = 0xFFFFFFFFu;
    rs.depth_func = 0x0201u;
    rs.depth_test = (u32)depth;
    rs.depth_write = (u32)depth;
    rsx_vertex_layout_plan layout;
    memset(&layout, 0, sizeof layout);
    layout.count = 2;
    layout.stride = sizeof(pm_vertex);
    return be->pipeline_create(NULL, kPmVS, ps, &rs, &layout, sizeof(pm_vertex),
                               RSX_BE_FMT_R8G8B8A8, 1);
}

static void draw(u32 pipeline, u32 target, u32 depth, const float col[4], float z,
                 u32 sx, u32 sw, u32 texture)
{
    pm_vertex v[3] = {
        { { -1, -1, z, 1 }, { col[0], col[1], col[2], col[3] } },
        { {  3, -1, z, 1 }, { col[0], col[1], col[2], col[3] } },
        { { -1,  3, z, 1 }, { col[0], col[1], col[2], col[3] } },
    };
    u32 tex[RSX_BE_MAX_TEXTURES] = { 0 };
    rsx_be_sampler_desc samp[RSX_BE_MAX_TEXTURES];
    memset(samp, 0, sizeof samp);
    samp[0].wrap_s = samp[0].wrap_t = samp[0].wrap_r = 3;
    tex[0] = texture;
    be->bind_targets(NULL, &target, 1, depth);
    be->bind_pipeline(NULL, pipeline);
    be->bind_textures(NULL, tex, samp, texture ? 1u : 0u);
    be->set_viewport(NULL, 0, 0, 64, 64);
    be->set_scissor(NULL, sx, 0, sw, 64);
    be->set_stencil_ref(NULL, 0);
    be->draw(NULL, RSX_TOPOLOGY_TRIANGLES, v, 3, sizeof(pm_vertex), NULL, 0);
}

static void test_merge(u32 a)
{
    const pm_counts c0 = counts();
    draw(solid, a, 0, kRed, 0.5f, 0, 64, 0);
    draw(solid, a, 0, kGreen, 0.5f, 0, 32, 0);
    const pm_counts c1 = counts();
    check(c1.opened - c0.opened == 1 && c1.merged - c0.merged == 1, "two draws into one target share a pass");
    check(pixel(a, 8, 32) == rgba(kGreen), "second draw inside its scissor");
    check(pixel(a, 56, 32) == rgba(kRed), "first draw outside the second's scissor");
}

static void test_feedback(u32 a)
{
    be->clear_color(NULL, a, (const float[4]){ 0, 0, 0, 0 });
    const pm_counts c0 = counts();
    draw(solid, a, 0, kBlue, 0.5f, 0, 64, 0);
    const float uv[4] = { 0.75f, 0.5f, 0, 1 };
    draw(textured, a, 0, uv, 0.5f, 0, 32, a);
    const pm_counts c1 = counts();
    check(c1.opened - c0.opened == 2 && c1.merged == c0.merged, "sampling a bound target ends the pass");
    check(pixel(a, 8, 32) == rgba(kBlue), "draw that samples its own target sees the previous draw");
}

static void test_clear_between(u32 a)
{
    const pm_counts c0 = counts();
    draw(solid, a, 0, kRed, 0.5f, 0, 64, 0);
    be->clear_color(NULL, a, kBlue);
    draw(solid, a, 0, kGreen, 0.5f, 0, 32, 0);
    const pm_counts c1 = counts();
    check(c1.opened - c0.opened == 2 && c1.merged == c0.merged, "a clear ends the pass");
    check(pixel(a, 56, 32) == rgba(kBlue), "clear lands after the first draw");
    check(pixel(a, 8, 32) == rgba(kGreen), "draw after the clear lands after it");
}

static void test_readback_between(u32 a)
{
    const pm_counts c0 = counts();
    draw(solid, a, 0, kRed, 0.5f, 0, 64, 0);
    const u32 mid = pixel(a, 8, 32);
    draw(solid, a, 0, kGreen, 0.5f, 0, 64, 0);
    const pm_counts c1 = counts();
    check(mid == rgba(kRed), "readback between draws sees the first");
    check(c1.opened - c0.opened == 2, "a readback ends the pass");
    check(pixel(a, 8, 32) == rgba(kGreen), "draw after a readback");
}

static void test_upload_between(u32 a)
{
    const u32 t = be->texture_create(NULL, RSX_BE_FMT_R8G8B8A8, 4, 4, 1, 1, 0xAAE4u, 0x85u);
    check(t != 0, "texture created");
    u8 yellow[64], cyan[64];
    for (u32 i = 0; i < 16; i++) {
        yellow[i * 4] = 0xFF; yellow[i * 4 + 1] = 0xFF; yellow[i * 4 + 2] = 0; yellow[i * 4 + 3] = 0xFF;
        cyan[i * 4] = 0; cyan[i * 4 + 1] = 0xFF; cyan[i * 4 + 2] = 0xFF; cyan[i * 4 + 3] = 0xFF;
    }
    const float uv[4] = { 0.5f, 0.5f, 0, 1 };
    be->texture_upload(NULL, t, 0, 0, 4, 4, yellow, 16, 4);
    const pm_counts c0 = counts();
    draw(textured, a, 0, uv, 0.5f, 0, 64, t);
    be->texture_upload(NULL, t, 0, 0, 4, 4, cyan, 16, 4);
    draw(textured, a, 0, uv, 0.5f, 0, 32, t);
    const pm_counts c1 = counts();
    check(c1.opened - c0.opened == 2, "an upload ends the pass");
    check(pixel(a, 56, 32) == rgba(kYellow), "draw before the upload samples the old texels");
    check(pixel(a, 8, 32) == rgba(kCyan), "draw after the upload samples the new texels");
    be->texture_release(NULL, t);
}

static void test_depth(u32 a, u32 z)
{
    be->clear_depth_stencil(NULL, z, RSX_BE_CLEAR_DEPTH | RSX_BE_CLEAR_STENCIL, 1.0f, 0);
    const pm_counts c0 = counts();
    draw(solid_depth, a, z, kRed, 0.5f, 0, 64, 0);
    draw(solid_depth, a, z, kGreen, 0.7f, 0, 64, 0);
    draw(solid_depth, a, z, kBlue, 0.3f, 0, 32, 0);
    const pm_counts c1 = counts();
    check(c1.opened - c0.opened == 1 && c1.merged - c0.merged == 2, "depth draws share a pass");
    check(pixel(a, 56, 32) == rgba(kRed), "farther draw fails the depth test inside the pass");
    check(pixel(a, 8, 32) == rgba(kBlue), "nearer draw passes the depth test inside the pass");
}

static void test_switch_targets(u32 a, u32 b)
{
    const pm_counts c0 = counts();
    draw(solid, a, 0, kRed, 0.5f, 0, 64, 0);
    draw(solid, b, 0, kGreen, 0.5f, 0, 64, 0);
    const float uv[4] = { 0.5f, 0.5f, 0, 1 };
    draw(textured, a, 0, uv, 0.5f, 0, 32, b);
    const pm_counts c1 = counts();
    check(c1.opened - c0.opened == 3, "each change of target opens a pass");
    check(pixel(a, 8, 32) == rgba(kGreen), "draw samples the other target's last draw");
    check(pixel(a, 56, 32) == rgba(kRed), "first target keeps its own draw");
}

static void test_merge_off(u32 a)
{
    s_pass.merge = 0;
    const pm_counts c0 = counts();
    draw(solid, a, 0, kRed, 0.5f, 0, 64, 0);
    draw(solid, a, 0, kGreen, 0.5f, 0, 32, 0);
    const pm_counts c1 = counts();
    check(c1.opened - c0.opened == 2 && c1.merged == c0.merged, "RSX_VK_PASS_MERGE=0 keeps one pass per draw");
    check(pixel(a, 8, 32) == rgba(kGreen) && pixel(a, 56, 32) == rgba(kRed), "same pixels without merging");
    s_pass.merge = 1;
}

int main(int argc, char** argv)
{
    (void)argc; (void)argv;
    setenv("RSX_VK_ASYNC_SHADERS", "0", 1);
    setenv("RSX_VK_SHADER_WARMUP", "0", 1);
    setenv("RSX_VK_SHADER_CACHE", "0", 1);
    setenv("PS3RECOMP_VK_GUEST_PROGRAMS", "1", 1);
    if (vk_init_all(64, 64, "render pass merge test")) { fprintf(stderr, "SKIP: no Vulkan device\n"); return 0; }
    if (!s_g.on) { fprintf(stderr, "SKIP: no guest program translation\n"); return 0; }
    if (be->init(NULL, 64, 64)) { fprintf(stderr, "FAIL: engine init\n"); return 1; }
    s_pass.merge = 1;
    solid = make_pipeline(kPmSolidPS, 0);
    solid_depth = make_pipeline(kPmSolidPS, 1);
    textured = make_pipeline(kPmTexturePS, 0);
    check(solid && solid_depth && textured, "pipelines built");
    const u32 a = be->color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, 64, 64, NULL, 0);
    const u32 b = be->color_target_create(NULL, RSX_BE_FMT_R8G8B8A8, 64, 64, NULL, 0);
    const u32 z = be->depth_target_create(NULL, 64, 64);
    check(a && b && z, "targets created");
    if (!failures) {
        test_merge(a);
        test_feedback(a);
        test_clear_between(a);
        test_readback_between(a);
        test_upload_between(a);
        test_depth(a, z);
        test_switch_targets(a, b);
        test_merge_off(a);
    }
    be->color_target_release(NULL, a);
    be->color_target_release(NULL, b);
    be->depth_target_release(NULL, z);
    be->shutdown(NULL);
    printf("vk_pass_merge: %d/%d checks passed\n", checks - failures, checks);
    return failures ? 1 : 0;
}
