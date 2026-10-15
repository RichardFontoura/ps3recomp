#include "rsx_vk_texture.spv.h"

static struct {
    int attempted;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipelineLayout layout;
    VkPipeline pipeline;
} s_tc;

static void vk_texture_compute_shutdown(void)
{
    if (s_tc.pipeline) pvkDestroyPipeline(s_vk.device, s_tc.pipeline, NULL);
    if (s_tc.layout) pvkDestroyPipelineLayout(s_vk.device, s_tc.layout, NULL);
    if (s_tc.pool) pvkDestroyDescriptorPool(s_vk.device, s_tc.pool, NULL);
    if (s_tc.set_layout) pvkDestroyDescriptorSetLayout(s_vk.device, s_tc.set_layout, NULL);
    memset(&s_tc, 0, sizeof s_tc);
}

static int vk_texture_compute_init(void)
{
    if (s_tc.attempted) return s_tc.pipeline != VK_NULL_HANDLE;
    s_tc.attempted = 1;
    const char* e = getenv("RSX_VK_TEXTURE_COMPUTE");
    if (e && !atoi(e)) return 0;
    VkPhysicalDeviceProperties props;
    pvkGetPhysicalDeviceProperties(s_vk.phys, &props);
    if (props.limits.maxStorageBufferRange < VK_BATCH_RING_BYTES) return 0;
    u32 n = 64;
    VkQueueFamilyProperties families[64];
    pvkGetPhysicalDeviceQueueFamilyProperties(s_vk.phys, &n, families);
    if (s_vk.queue_family >= n || !(families[s_vk.queue_family].queueFlags & VK_QUEUE_COMPUTE_BIT))
        return 0;
    if (vk_batch_ring()) return 0;
    VkDescriptorSetLayoutBinding binding = { .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1, .pBindings = &binding };
    if (pvkCreateDescriptorSetLayout(s_vk.device, &sci, NULL, &s_tc.set_layout) != VK_SUCCESS) goto fail;
    VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size };
    if (pvkCreateDescriptorPool(s_vk.device, &pci, NULL, &s_tc.pool) != VK_SUCCESS) goto fail;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = s_tc.pool, .descriptorSetCount = 1, .pSetLayouts = &s_tc.set_layout };
    if (pvkAllocateDescriptorSets(s_vk.device, &dai, &s_tc.set) != VK_SUCCESS) goto fail;
    VkDescriptorBufferInfo bi = { s_vb.ring.buf, 0, VK_BATCH_RING_BYTES };
    VkWriteDescriptorSet write = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = s_tc.set, .dstBinding = 0, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi };
    pvkUpdateDescriptorSets(s_vk.device, 1, &write, 0, NULL);
    VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 11 * sizeof(u32) };
    VkPipelineLayoutCreateInfo lci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &s_tc.set_layout,
        .pushConstantRangeCount = 1, .pPushConstantRanges = &push };
    if (pvkCreatePipelineLayout(s_vk.device, &lci, NULL, &s_tc.layout) != VK_SUCCESS) goto fail;
    VkShaderModule module = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo mci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof rsx_vk_texture_spv, .pCode = rsx_vk_texture_spv };
    if (pvkCreateShaderModule(s_vk.device, &mci, NULL, &module) != VK_SUCCESS) goto fail;
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main" },
        .layout = s_tc.layout };
    VkResult result = pvkCreateComputePipelines(s_vk.device, VK_NULL_HANDLE, 1, &cpi, NULL, &s_tc.pipeline);
    pvkDestroyShaderModule(s_vk.device, module, NULL);
    if (result != VK_SUCCESS) goto fail;
    VK_LOG("texture conversion: GPU compute enabled\n");
    return 1;
fail:
    vk_texture_compute_shutdown();
    s_tc.attempted = 1;
    VK_LOG("texture conversion: compute unavailable, using CPU\n");
    return 0;
}

static void vk_texture_compute_dispatch(VkDeviceSize input, VkDeviceSize output, VkDeviceSize dst_bytes,
                                        u32 w, u32 h, u32 pitch, const rsx_tex_layout* tl,
                                        u32 format, u32 source)
{
    u32 push[11] = { (u32)(input / 4), (u32)(output / 4), w, h, pitch,
        tl->bytes_per_texel, tl->dst_bytes_per_texel, format,
        tl->swizzled, (u32)rsx_texture_argb_is_rgba(), source };
    pvkCmdBindPipeline(s_vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_tc.pipeline);
    pvkCmdBindDescriptorSets(s_vk.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s_tc.layout,
        0, 1, &s_tc.set, 0, NULL);
    pvkCmdPushConstants(s_vk.cmd, s_tc.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
    const u32 groups = (u32)((dst_bytes + 255u) / 256u);
    const u32 gx = groups > 65535u ? 65535u : groups;
    pvkCmdDispatch(s_vk.cmd, gx, (groups + gx - 1u) / gx, 1);
    VkBufferMemoryBarrier barrier = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = s_vb.ring.buf, .offset = output, .size = dst_bytes };
    pvkCmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 1, &barrier, 0, NULL);
}

static int vk_texture_compute(const u8* src, u32 w, u32 h, const rsx_tex_layout* tl,
                               VkDeviceSize* output)
{
    if (tl->compressed || !w || !h || !vk_texture_compute_init()) return 0;
    const VkDeviceSize src_bytes = ((VkDeviceSize)tl->face_bytes + 15u) & ~15ull;
    const VkDeviceSize dst_bytes = ((VkDeviceSize)tl->dst_row_bytes * tl->rows + 15u) & ~15ull;
    if (!src_bytes || !dst_bytes || vk_batch_reserve(src_bytes + dst_bytes, 0)) return 0;
    void* in; void* out;
    VkDeviceSize input = vk_batch_alloc(src_bytes, 16, &in);
    *output = vk_batch_alloc(dst_bytes, 16, &out);
    memcpy(in, src, tl->face_bytes);
    memset((u8*)in + tl->face_bytes, 0, (size_t)(src_bytes - tl->face_bytes));
    if (vk_begin()) return -1;
    vk_texture_compute_dispatch(input, *output, dst_bytes, w, h, tl->row_bytes, tl,
                                tl->base_format, 0);
    return 1;
}
