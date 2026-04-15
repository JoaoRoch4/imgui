#include "vulkan_emoji_atlas.hpp"
#include "imgui_impl_vulkan.h"

#include <array>
#include <bit>
#include <cstring>
#include <print>

// ── VulkanEmojiAtlas ──────────────────────────────────────────────────────────

VulkanEmojiAtlas::VulkanEmojiAtlas(VulkanContext& ctx)
    : Ctx_ { ctx }
{
}

VulkanEmojiAtlas::~VulkanEmojiAtlas()
{
    Cleanup();
}

void VulkanEmojiAtlas::Cleanup()
{
    VkDevice dev   = Ctx_.Device;
    const VkAllocationCallbacks* alloc = Ctx_.Allocator;

    if (DescSet_ != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(DescSet_);
        DescSet_ = VK_NULL_HANDLE;
    }
    if (Sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(dev, Sampler_, alloc);
        Sampler_ = VK_NULL_HANDLE;
    }
    if (ImageView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(dev, ImageView_, alloc);
        ImageView_ = VK_NULL_HANDLE;
    }
    if (Image_ != VK_NULL_HANDLE) {
        vkDestroyImage(dev, Image_, alloc);
        Image_ = VK_NULL_HANDLE;
    }
    if (Memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(dev, Memory_, alloc);
        Memory_ = VK_NULL_HANDLE;
    }
    TextureID_ = ImTextureID_Invalid;
}

uint32_t VulkanEmojiAtlas::FindMemoryType(uint32_t type_filter,
                                           VkMemoryPropertyFlags props) const
{
    VkPhysicalDeviceMemoryProperties mem_props {};
    vkGetPhysicalDeviceMemoryProperties(Ctx_.PhysicalDevice, &mem_props);
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    IM_ASSERT(false && "VulkanEmojiAtlas: no suitable memory type found");
    return 0;
}

ImTextureID VulkanEmojiAtlas::UploadRGBA(const std::vector<uint8_t>& pixels,
                                          int width, int height)
{
    VkDevice     dev   = Ctx_.Device;
    VkQueue      queue = Ctx_.Queue;
    const VkAllocationCallbacks* alloc = Ctx_.Allocator;
    VkResult     err {};

    const VkDeviceSize img_size =
        static_cast<VkDeviceSize>(width * height * 4);

    // ── Staging buffer ────────────────────────────────────────────────────────
    VkBuffer       staging_buf    { VK_NULL_HANDLE };
    VkDeviceMemory staging_mem    { VK_NULL_HANDLE };

    {
        VkBufferCreateInfo ci {};
        ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        ci.size        = img_size;
        ci.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        err = vkCreateBuffer(dev, &ci, alloc, &staging_buf);
        VulkanContext::CheckVkResult(err);
    }
    {
        VkMemoryRequirements req {};
        vkGetBufferMemoryRequirements(dev, staging_buf, &req);

        VkMemoryAllocateInfo ai {};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = FindMemoryType(
            req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        err = vkAllocateMemory(dev, &ai, alloc, &staging_mem);
        VulkanContext::CheckVkResult(err);
        vkBindBufferMemory(dev, staging_buf, staging_mem, 0);
    }
    {
        void* mapped { nullptr };
        vkMapMemory(dev, staging_mem, 0, img_size, 0, &mapped);
        std::memcpy(mapped, pixels.data(), static_cast<size_t>(img_size));
        vkUnmapMemory(dev, staging_mem);
    }

    // ── Device-local image ────────────────────────────────────────────────────
    {
        VkImageCreateInfo ci {};
        ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.format        = VK_FORMAT_R8G8B8A8_UNORM;
        ci.extent        = { static_cast<uint32_t>(width),
                             static_cast<uint32_t>(height), 1 };
        ci.mipLevels     = 1;
        ci.arrayLayers   = 1;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ci.usage         = VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        err = vkCreateImage(dev, &ci, alloc, &Image_);
        VulkanContext::CheckVkResult(err);
    }
    {
        VkMemoryRequirements req {};
        vkGetImageMemoryRequirements(dev, Image_, &req);

        VkMemoryAllocateInfo ai {};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = req.size;
        ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        err = vkAllocateMemory(dev, &ai, alloc, &Memory_);
        VulkanContext::CheckVkResult(err);
        vkBindImageMemory(dev, Image_, Memory_, 0);
    }

    // ── One-shot command buffer: layout transition + copy ────────────────────
    VkCommandPool   cmd_pool { VK_NULL_HANDLE };
    VkCommandBuffer cmd_buf  { VK_NULL_HANDLE };

    {
        VkCommandPoolCreateInfo ci {};
        ci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        ci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = Ctx_.QueueFamily;
        err = vkCreateCommandPool(dev, &ci, alloc, &cmd_pool);
        VulkanContext::CheckVkResult(err);
    }
    {
        VkCommandBufferAllocateInfo ai {};
        ai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool        = cmd_pool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        err = vkAllocateCommandBuffers(dev, &ai, &cmd_buf);
        VulkanContext::CheckVkResult(err);
    }
    {
        VkCommandBufferBeginInfo bi {};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd_buf, &bi);
    }

    // Transition UNDEFINED → TRANSFER_DST_OPTIMAL
    {
        VkImageMemoryBarrier barrier {};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask                   = 0;
        barrier.dstAccessMask                   = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout                       = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout                       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                           = Image_;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;
        vkCmdPipelineBarrier(cmd_buf,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    // Copy staging buffer → image
    {
        VkBufferImageCopy region {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent                 = { static_cast<uint32_t>(width),
                                               static_cast<uint32_t>(height), 1 };
        vkCmdCopyBufferToImage(cmd_buf, staging_buf, Image_,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    // Transition TRANSFER_DST_OPTIMAL → SHADER_READ_ONLY_OPTIMAL
    {
        VkImageMemoryBarrier barrier {};
        barrier.sType                           = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask                   = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask                   = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout                       = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout                       = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex             = VK_QUEUE_FAMILY_IGNORED;
        barrier.image                           = Image_;
        barrier.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel   = 0;
        barrier.subresourceRange.levelCount     = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount     = 1;
        vkCmdPipelineBarrier(cmd_buf,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    vkEndCommandBuffer(cmd_buf);

    // Submit + wait idle
    {
        VkSubmitInfo si {};
        si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &cmd_buf;
        vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
    }

    // Clean up one-shot resources
    vkFreeCommandBuffers(dev, cmd_pool, 1, &cmd_buf);
    vkDestroyCommandPool(dev, cmd_pool, alloc);
    vkDestroyBuffer(dev, staging_buf, alloc);
    vkFreeMemory(dev, staging_mem, alloc);

    // ── Image view + sampler ──────────────────────────────────────────────────
    {
        VkImageViewCreateInfo ci {};
        ci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image                           = Image_;
        ci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        ci.format                          = VK_FORMAT_R8G8B8A8_UNORM;
        ci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        ci.subresourceRange.levelCount     = 1;
        ci.subresourceRange.layerCount     = 1;
        err = vkCreateImageView(dev, &ci, alloc, &ImageView_);
        VulkanContext::CheckVkResult(err);
    }
    {
        VkSamplerCreateInfo ci {};
        ci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        ci.magFilter    = VK_FILTER_LINEAR;
        ci.minFilter    = VK_FILTER_LINEAR;
        ci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        ci.maxLod       = VK_LOD_CLAMP_NONE;
        err = vkCreateSampler(dev, &ci, alloc, &Sampler_);
        VulkanContext::CheckVkResult(err);
    }

    // ── Register with ImGui Vulkan backend ────────────────────────────────────
    DescSet_ = ImGui_ImplVulkan_AddTexture(
        Sampler_, ImageView_,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // VkDescriptorSet is a 64-bit non-dispatchable handle (pointer on 64-bit
    // platforms); ImTextureID is uint64_t — same size, trivially copyable.
    return std::bit_cast<ImTextureID>(DescSet_);
}
