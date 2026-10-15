#include "../rsx_vulkan_backend.c"

volatile int g_ps3_timeline_on;
void ps3_timeline_event(uint32_t t, uint32_t a) { (void)t; (void)a; }

#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

static int init_test(void)
{
    if (vk_load_library()) return -1;
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "texture differential test", .apiVersion = VK_API_VERSION_1_0 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    if (pvkCreateInstance(&ici, NULL, &s_vk.instance) != VK_SUCCESS ||
        vk_load_instance_functions() || vk_pick_device()) return -1;
    float priority = 1;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = s_vk.queue_family, .queueCount = 1, .pQueuePriorities = &priority };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    if (pvkCreateDevice(s_vk.phys, &dci, NULL, &s_vk.device) != VK_SUCCESS || vk_load_device_functions()) return -1;
    pvkGetDeviceQueue(s_vk.device, s_vk.queue_family, 0, &s_vk.queue);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = s_vk.queue_family };
    if (pvkCreateCommandPool(s_vk.device, &pci, NULL, &s_vk.pool) != VK_SUCCESS) return -1;
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = s_vk.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    if (pvkAllocateCommandBuffers(s_vk.device, &ai, &s_vk.cmd) != VK_SUCCESS) return -1;
    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    return pvkCreateFence(s_vk.device, &fi, NULL, &s_vk.fence) == VK_SUCCESS ? 0 : -1;
}

int main(int argc, char** argv)
{
    (void)argv;
    setenv("RSX_VK_TEXTURE_COMPUTE", "1", 1);
    if (init_test()) return 77;
    REQUIRE(vk_texture_compute_init());
    const u32 formats[] = {0x81,0x82,0x83,0x84,0x85,0x8b,0x8c,0x8d,0x90,
                           0x92,0x94,0x95,0x97,0x9a,0x9b,0x9c,0x9d,0x9e,0x9f};
    const u32 dims[][2] = {{1,1},{3,5},{32,16},{16,32},{128,64}};
    u32 tests = 0;
    for (u32 f = 0; f < sizeof formats / sizeof formats[0]; f++) {
        for (u32 d = 0; d < sizeof dims / sizeof dims[0]; d++) {
            for (u32 linear = 0; linear < 2; linear++) {
                const u32 w = dims[d][0], h = dims[d][1];
                rsx_tex_layout tl;
                rsx_texture_layout(formats[f] | (linear ? 0x20 : 0), w, h, &tl);
                if (linear) rsx_texture_layout_pitched(formats[f] | 0x20, w, h, tl.row_bytes + 7, &tl);
                u8* src = malloc(tl.face_bytes);
                const size_t bytes = (size_t)tl.dst_row_bytes * tl.rows;
                u8* expected = malloc(bytes);
                REQUIRE(src && expected);
                u32 rng = 42;
                for (u32 i = 0; i < tl.face_bytes; i++) {
                    rng = rng * 1664525u + 1013904223u; src[i] = (u8)(rng >> 24);
                }
                rsx_texture_decode(expected, tl.dst_row_bytes, src, w, h, &tl, rsx_texture_argb_is_rgba());
                VkDeviceSize off;
                REQUIRE(vk_texture_compute(src, w, h, &tl, &off) == 1);
                memset(src, 0xcc, tl.face_bytes);
                VkMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
                pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                    0, 1, &barrier, 0, NULL, 0, NULL);
                REQUIRE(vk_flush() == 0);
                const u8* actual = (const u8*)s_vb.ring.ptr + off;
                if (memcmp(actual, expected, bytes)) {
                    for (size_t i = 0; i < bytes; i++) if (actual[i] != expected[i]) {
                        fprintf(stderr, "format=%02x %ux%u linear=%u byte=%zu GPU=%u CPU=%u\n",
                            formats[f], w, h, linear, i, actual[i], expected[i]); break;
                    }
                    return 1;
                }
                free(expected); free(src); tests++;
            }
        }
    }
    printf("GPU texture differential: %u/%u passed (TEX_RGBA=%d)\n", tests, tests, rsx_texture_argb_is_rgba());
    {
        rsx_tex_layout tl;
        rsx_texture_layout(0x85, 32, 16, &tl);
        u8 src[2048], expected[8][2048];
        VkDeviceSize offsets[8];
        for (u32 j = 0; j < 8; j++) {
            for (u32 i = 0; i < sizeof src; i++) src[i] = (u8)(i * 37 + j * 19);
            rsx_texture_decode(expected[j], tl.dst_row_bytes, src, 32, 16, &tl, rsx_texture_argb_is_rgba());
            REQUIRE(vk_texture_compute(src, 32, 16, &tl, &offsets[j]) == 1);
        }
        VkMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
        pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
            0, 1, &b, 0, NULL, 0, NULL);
        REQUIRE(vk_flush() == 0);
        for (u32 j = 0; j < 8; j++) REQUIRE(!memcmp((u8*)s_vb.ring.ptr + offsets[j], expected[j], 2048));
        puts("GPU batch lifetime: 8/8 passed");
    }
    if (argc > 1) {
        rsx_tex_layout tl;
        rsx_texture_layout(0x85, 1024, 1024, &tl);
        u8* src = calloc(1, tl.face_bytes); u8* dst = malloc(tl.dst_row_bytes * tl.rows);
        REQUIRE(src && dst);
        const int n = 30;
        u64 start = vk_probe_now();
        for (int i = 0; i < n; i++) rsx_texture_decode(dst, tl.dst_row_bytes, src, 1024, 1024, &tl, 0);
        double cpu_ms = (vk_probe_now() - start) / (1e6 * n);
        start = vk_probe_now();
        for (int i = 0; i < n; i++) {
            VkDeviceSize off;
            REQUIRE(vk_texture_compute(src, 1024, 1024, &tl, &off) == 1);
            REQUIRE(vk_flush() == 0);
        }
        printf("1024x1024 ARGB Morton: CPU %.3f ms, GPU+copy+fence %.3f ms\n",
            cpu_ms, (vk_probe_now() - start) / (1e6 * n));
        free(src); free(dst);
    }
    vk_texture_compute_shutdown();
    vk_destroy_buffer(&s_vb.ring);
    pvkDestroyFence(s_vk.device, s_vk.fence, NULL);
    pvkDestroyCommandPool(s_vk.device, s_vk.pool, NULL);
    pvkDestroyDevice(s_vk.device, NULL);
    pvkDestroyInstance(s_vk.instance, NULL);
    dlclose(s_vk.lib);
    return 0;
}
