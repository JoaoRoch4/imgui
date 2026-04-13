// VOLK_IMPLEMENTATION must be defined before any header that includes <volk.h>.
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
#define VOLK_IMPLEMENTATION
#endif

#include "vulkan_context.hpp"
#include <stdio.h>   // fprintf
#include <stdlib.h>  // abort, exit
#include <string.h>  // strcmp

// ── static helpers ────────────────────────────────────────────────────────────

void VulkanContext::CheckVkResult(VkResult err)
{
    if (err == VK_SUCCESS)
        return;
    fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    if (err < 0)
        abort();
}

#ifdef APP_USE_VULKAN_DEBUG_REPORT
VKAPI_ATTR VkBool32 VKAPI_CALL VulkanContext::DebugReportCallback(
    VkDebugReportFlagsEXT /*flags*/, VkDebugReportObjectTypeEXT objectType,
    uint64_t /*object*/, size_t /*location*/, int32_t /*messageCode*/,
    const char* /*pLayerPrefix*/, const char* pMessage, void* /*pUserData*/)
{
    fprintf(stderr, "[vulkan] Debug report from ObjectType: %i\nMessage: %s\n\n",
            objectType, pMessage);
    return VK_FALSE;
}
#endif

bool VulkanContext::IsExtensionAvailable(const ImVector<VkExtensionProperties>& properties,
                                         const char* extension)
{
    for (const VkExtensionProperties& p : properties)
        if (strcmp(p.extensionName, extension) == 0)
            return true;
    return false;
}

// ── Setup / Cleanup ───────────────────────────────────────────────────────────

void VulkanContext::Setup(ImVector<const char*> instance_extensions)
{
    VkResult err;
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
    volkInitialize();
#endif

    // Create Vulkan Instance
    {
        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        err = vkEnumerateInstanceExtensionProperties(nullptr, &properties_count, properties.Data);
        CheckVkResult(err);

        if (IsExtensionAvailable(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
        {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif
#ifdef APP_USE_VULKAN_DEBUG_REPORT
        const char* layers[] = { "VK_LAYER_KHRONOS_validation" };
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = layers;
        instance_extensions.push_back("VK_EXT_debug_report");
#endif
        create_info.enabledExtensionCount  = (uint32_t)instance_extensions.Size;
        create_info.ppEnabledExtensionNames = instance_extensions.Data;
        err = vkCreateInstance(&create_info, Allocator, &Instance);
        CheckVkResult(err);
#ifdef IMGUI_IMPL_VULKAN_USE_VOLK
        volkLoadInstance(Instance);
#endif
#ifdef APP_USE_VULKAN_DEBUG_REPORT
        auto f_vkCreateDebugReportCallbackEXT =
            (PFN_vkCreateDebugReportCallbackEXT)vkGetInstanceProcAddr(Instance, "vkCreateDebugReportCallbackEXT");
        IM_ASSERT(f_vkCreateDebugReportCallbackEXT != nullptr);
        VkDebugReportCallbackCreateInfoEXT debug_report_ci = {};
        debug_report_ci.sType    = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
        debug_report_ci.flags    = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT;
        debug_report_ci.pfnCallback = DebugReportCallback;
        err = f_vkCreateDebugReportCallbackEXT(Instance, &debug_report_ci, Allocator, &DebugReport);
        CheckVkResult(err);
#endif
    }

    // Select Physical Device (GPU)
    PhysicalDevice = ImGui_ImplVulkanH_SelectPhysicalDevice(Instance);
    IM_ASSERT(PhysicalDevice != VK_NULL_HANDLE);

    // Select graphics queue family
    QueueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(PhysicalDevice);
    IM_ASSERT(QueueFamily != (uint32_t)-1);

    // Create Logical Device (with 1 queue)
    {
        ImVector<const char*> device_extensions;
        device_extensions.push_back("VK_KHR_swapchain");

        uint32_t properties_count;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateDeviceExtensionProperties(PhysicalDevice, nullptr, &properties_count, nullptr);
        properties.resize(properties_count);
        vkEnumerateDeviceExtensionProperties(PhysicalDevice, nullptr, &properties_count, properties.Data);
#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
            device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
#endif
        const float queue_priority[] = { 1.0f };
        VkDeviceQueueCreateInfo queue_info[1] = {};
        queue_info[0].sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info[0].queueFamilyIndex = QueueFamily;
        queue_info[0].queueCount       = 1;
        queue_info[0].pQueuePriorities = queue_priority;
        VkDeviceCreateInfo create_info = {};
        create_info.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount    = sizeof(queue_info) / sizeof(queue_info[0]);
        create_info.pQueueCreateInfos       = queue_info;
        create_info.enabledExtensionCount   = (uint32_t)device_extensions.Size;
        create_info.ppEnabledExtensionNames = device_extensions.Data;
        err = vkCreateDevice(PhysicalDevice, &create_info, Allocator, &Device);
        CheckVkResult(err);
        vkGetDeviceQueue(Device, QueueFamily, 0, &Queue);
    }

    // Create Descriptor Pool
    {
        VkDescriptorPoolSize pool_sizes[] =
        {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, IMGUI_IMPL_VULKAN_MINIMUM_IMAGE_SAMPLER_POOL_SIZE },
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets       = 0;
        for (VkDescriptorPoolSize& s : pool_sizes)
            pool_info.maxSets += s.descriptorCount;
        pool_info.poolSizeCount = (uint32_t)IM_COUNTOF(pool_sizes);
        pool_info.pPoolSizes    = pool_sizes;
        err = vkCreateDescriptorPool(Device, &pool_info, Allocator, &DescriptorPool);
        CheckVkResult(err);
    }
}

void VulkanContext::SetupWindow(VkSurfaceKHR surface, int width, int height)
{
    ImGui_ImplVulkanH_Window* wd = &MainWindowData;

    VkBool32 res;
    vkGetPhysicalDeviceSurfaceSupportKHR(PhysicalDevice, QueueFamily, surface, &res);
    if (res != VK_TRUE)
    {
        fprintf(stderr, "Error no WSI support on physical device 0\n");
        exit(-1);
    }

    const VkFormat requestSurfaceImageFormat[] = {
        VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8_UNORM,   VK_FORMAT_R8G8B8_UNORM,
    };
    const VkColorSpaceKHR requestSurfaceColorSpace = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    wd->Surface       = surface;
    wd->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        PhysicalDevice, wd->Surface,
        requestSurfaceImageFormat, (size_t)IM_COUNTOF(requestSurfaceImageFormat),
        requestSurfaceColorSpace);

#ifdef APP_USE_UNLIMITED_FRAME_RATE
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR };
#else
    VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_FIFO_KHR };
#endif
    wd->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
        PhysicalDevice, wd->Surface, &present_modes[0], IM_COUNTOF(present_modes));

    IM_ASSERT(MinImageCount >= 2);
    ImGui_ImplVulkanH_CreateOrResizeWindow(
        Instance, PhysicalDevice, Device, wd,
        QueueFamily, Allocator, width, height, MinImageCount, 0);
}

void VulkanContext::Cleanup()
{
    vkDestroyDescriptorPool(Device, DescriptorPool, Allocator);
#ifdef APP_USE_VULKAN_DEBUG_REPORT
    auto f_vkDestroyDebugReportCallbackEXT =
        (PFN_vkDestroyDebugReportCallbackEXT)vkGetInstanceProcAddr(Instance, "vkDestroyDebugReportCallbackEXT");
    f_vkDestroyDebugReportCallbackEXT(Instance, DebugReport, Allocator);
#endif
    vkDestroyDevice(Device, Allocator);
    vkDestroyInstance(Instance, Allocator);
}

void VulkanContext::CleanupWindow()
{
    ImGui_ImplVulkanH_DestroyWindow(Instance, Device, &MainWindowData, Allocator);
    vkDestroySurfaceKHR(Instance, MainWindowData.Surface, Allocator);
}

void VulkanContext::WaitIdle()
{
    VkResult err = vkDeviceWaitIdle(Device);
    CheckVkResult(err);
}

// ── Per-frame render / present ────────────────────────────────────────────────

void VulkanContext::FrameRender(ImDrawData* draw_data)
{
    ImGui_ImplVulkanH_Window* wd = &MainWindowData;
    VkSemaphore image_acquired_semaphore  = wd->FrameSemaphores[wd->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkResult err = vkAcquireNextImageKHR(Device, wd->Swapchain, UINT64_MAX,
                                          image_acquired_semaphore, VK_NULL_HANDLE, &wd->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        CheckVkResult(err);

    ImGui_ImplVulkanH_Frame* fd = &wd->Frames[wd->FrameIndex];
    {
        err = vkWaitForFences(Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
        CheckVkResult(err);
        err = vkResetFences(Device, 1, &fd->Fence);
        CheckVkResult(err);
    }
    {
        err = vkResetCommandPool(Device, fd->CommandPool, 0);
        CheckVkResult(err);
        VkCommandBufferBeginInfo info = {};
        info.sType  = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        err = vkBeginCommandBuffer(fd->CommandBuffer, &info);
        CheckVkResult(err);
    }
    {
        VkRenderPassBeginInfo info = {};
        info.sType                    = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass               = wd->RenderPass;
        info.framebuffer              = fd->Framebuffer;
        info.renderArea.extent.width  = wd->Width;
        info.renderArea.extent.height = wd->Height;
        info.clearValueCount          = 1;
        info.pClearValues             = &wd->ClearValue;
        vkCmdBeginRenderPass(fd->CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    ImGui_ImplVulkan_RenderDrawData(draw_data, fd->CommandBuffer);

    vkCmdEndRenderPass(fd->CommandBuffer);
    {
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo info = {};
        info.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        info.waitSemaphoreCount   = 1;
        info.pWaitSemaphores      = &image_acquired_semaphore;
        info.pWaitDstStageMask    = &wait_stage;
        info.commandBufferCount   = 1;
        info.pCommandBuffers      = &fd->CommandBuffer;
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores    = &render_complete_semaphore;
        err = vkEndCommandBuffer(fd->CommandBuffer);
        CheckVkResult(err);
        err = vkQueueSubmit(Queue, 1, &info, fd->Fence);
        CheckVkResult(err);
    }
}

void VulkanContext::FramePresent()
{
    ImGui_ImplVulkanH_Window* wd = &MainWindowData;
    if (SwapChainRebuild)
        return;
    VkSemaphore render_complete_semaphore = wd->FrameSemaphores[wd->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info = {};
    info.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores    = &render_complete_semaphore;
    info.swapchainCount     = 1;
    info.pSwapchains        = &wd->Swapchain;
    info.pImageIndices      = &wd->FrameIndex;
    VkResult err = vkQueuePresentKHR(Queue, &info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
        SwapChainRebuild = true;
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
        return;
    if (err != VK_SUBOPTIMAL_KHR)
        CheckVkResult(err);
    wd->SemaphoreIndex = (wd->SemaphoreIndex + 1) % wd->SemaphoreCount;
}

// ── Utility ───────────────────────────────────────────────────────────────────

void VulkanContext::RebuildSwapchainIfNeeded(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    if (!SwapChainRebuild &&
        MainWindowData.Width  == width &&
        MainWindowData.Height == height)
        return;
    ImGui_ImplVulkan_SetMinImageCount(MinImageCount);
    ImGui_ImplVulkanH_CreateOrResizeWindow(
        Instance, PhysicalDevice, Device, &MainWindowData,
        QueueFamily, Allocator, width, height, MinImageCount, 0);
    MainWindowData.FrameIndex = 0;
    SwapChainRebuild = false;
}

void VulkanContext::SetClearColor(ImVec4 color)
{
    MainWindowData.ClearValue.color.float32[0] = color.x * color.w;
    MainWindowData.ClearValue.color.float32[1] = color.y * color.w;
    MainWindowData.ClearValue.color.float32[2] = color.z * color.w;
    MainWindowData.ClearValue.color.float32[3] = color.w;
}

ImGui_ImplVulkan_InitInfo VulkanContext::MakeInitInfo() const
{
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance                       = Instance;
    init_info.PhysicalDevice                 = PhysicalDevice;
    init_info.Device                         = Device;
    init_info.QueueFamily                    = QueueFamily;
    init_info.Queue                          = Queue;
    init_info.PipelineCache                  = PipelineCache;
    init_info.DescriptorPool                 = DescriptorPool;
    init_info.MinImageCount                  = MinImageCount;
    init_info.ImageCount                     = MainWindowData.ImageCount;
    init_info.Allocator                      = Allocator;
    init_info.PipelineInfoMain.RenderPass    = MainWindowData.RenderPass;
    init_info.PipelineInfoMain.Subpass       = 0;
    init_info.PipelineInfoMain.MSAASamples   = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn                = CheckVkResult;
    return init_info;
}
