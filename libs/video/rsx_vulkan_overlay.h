#ifndef PS3RECOMP_RSX_VULKAN_OVERLAY_H
#define PS3RECOMP_RSX_VULKAN_OVERLAY_H

#include <SDL2/SDL.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rsx_vk_overlay_context {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    uint32_t                  api_version;
    VkInstance                instance;
    VkPhysicalDevice          physical_device;
    VkDevice                  device;
    uint32_t                  queue_family;
    VkQueue                   queue;
    SDL_Window*               window;
    const char*               device_name;
} rsx_vk_overlay_context;

typedef struct rsx_vk_overlay_swapchain {
    VkFormat         format;
    VkExtent2D       extent;
    uint32_t         min_image_count;
    uint32_t         image_count;
    const VkImage*   images;
    VkPresentModeKHR present_mode;
} rsx_vk_overlay_swapchain;

typedef struct rsx_vk_overlay_hooks {
    void (*swapchain_ready)(const rsx_vk_overlay_context* context,
                            const rsx_vk_overlay_swapchain* swapchain, void* user);
    void (*swapchain_lost)(void* user);
    int  (*record)(VkCommandBuffer cmd, uint32_t image_index, void* user);
    int  (*event)(const SDL_Event* event, void* user);
    void* user;
} rsx_vk_overlay_hooks;

int         rsx_vulkan_overlay_register(const rsx_vk_overlay_hooks* hooks);
void        rsx_vulkan_backend_set_vsync(int on);
int         rsx_vulkan_backend_vsync(void);
SDL_Window* rsx_vulkan_backend_window(void);
const char* rsx_vulkan_backend_device_name(void);

#ifdef __cplusplus
}
#endif
#endif
