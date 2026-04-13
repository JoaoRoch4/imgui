#pragma once

#include "imgui.h"
#include "imgui_impl_vulkan.h"

//#define APP_USE_UNLIMITED_FRAME_RATE
#ifdef _DEBUG
#define APP_USE_VULKAN_DEBUG_REPORT
#endif

// VulkanContext owns all raw Vulkan state: instance, device, swapchain, frame render/present.
class VulkanContext
{
public:
    VkAllocationCallbacks*   Allocator        = nullptr;
    VkInstance               Instance         = VK_NULL_HANDLE;
    VkPhysicalDevice         PhysicalDevice   = VK_NULL_HANDLE;
    VkDevice                 Device           = VK_NULL_HANDLE;
    uint32_t                 QueueFamily      = static_cast<uint32_t>(-1);
    VkQueue                  Queue            = VK_NULL_HANDLE;
    VkPipelineCache          PipelineCache    = VK_NULL_HANDLE;
    VkDescriptorPool         DescriptorPool   = VK_NULL_HANDLE;
    ImGui_ImplVulkanH_Window MainWindowData   = {};
    uint32_t                 MinImageCount    = 2;
    bool                     SwapChainRebuild = false;

    // Initialise the Vulkan instance, device, queues and descriptor pool.
    void Setup(ImVector<const char*> instance_extensions);

    // Create swapchain, render pass, framebuffers for a given surface + size.
    void SetupWindow(VkSurfaceKHR surface, int width, int height);

    // Destroy device / instance resources.
    void Cleanup();

    // Destroy swapchain / surface resources.
    void CleanupWindow();

    // Block until the device is idle (call before shutdown).
    void WaitIdle();

    // Record + submit a frame's draw data to the GPU.
    void FrameRender(ImDrawData* draw_data);

    // Present the rendered frame and advance the semaphore index.
    void FramePresent();

    // Recreate the swapchain when the window is resized.  No-op if size unchanged.
    void RebuildSwapchainIfNeeded(int width, int height);

    // Convert a linear clear colour and store it in the window data.
    void SetClearColor(ImVec4 color);

    // Return a fully-populated ImGui_ImplVulkan_InitInfo ready for ImGui_ImplVulkan_Init().
    ImGui_ImplVulkan_InitInfo MakeInitInfo() const;

    // VkResult error handler used as ImGui_ImplVulkan_InitInfo::CheckVkResultFn.
    static void CheckVkResult(VkResult err);

private:
    static bool IsExtensionAvailable(const ImVector<VkExtensionProperties>& properties,
                                     const char* extension);

#ifdef APP_USE_VULKAN_DEBUG_REPORT
    VkDebugReportCallbackEXT DebugReport = VK_NULL_HANDLE;
    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugReportCallback(
        VkDebugReportFlagsEXT flags, VkDebugReportObjectTypeEXT objectType,
        uint64_t object, size_t location, int32_t messageCode,
        const char* pLayerPrefix, const char* pMessage, void* pUserData);
#endif
};
