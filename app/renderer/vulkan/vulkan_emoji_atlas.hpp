#pragma once

#include "app/ui/emoji_atlas.hpp"
#include "app/renderer/vulkan/vulkan_context.hpp"
#include <vulkan/vulkan.h>

// VulkanEmojiAtlas uploads the EmojiAtlas pixel data as a VkImage+VkImageView
// and registers it with the ImGui Vulkan backend via ImGui_ImplVulkan_AddTexture().
// The resulting ImTextureID is a VkDescriptorSet usable in ImGui::Image() calls.
//
// Lifetime: must outlive any frames that reference it.  Call Cleanup() (or let
// the destructor run) before destroying the VulkanContext.
class VulkanEmojiAtlas : public EmojiAtlas
{
public:
    explicit VulkanEmojiAtlas(VulkanContext& ctx);
    ~VulkanEmojiAtlas() override;

    // Release all Vulkan resources created during UploadRGBA().
    // Safe to call more than once.
    void Cleanup();

protected:
    ImTextureID UploadRGBA(const std::vector<uint8_t>& pixels,
                           int width, int height) override;

private:
    // Locate a device-memory type satisfying both filter and property requirements.
    [[nodiscard]] uint32_t FindMemoryType(uint32_t type_filter,
                                          VkMemoryPropertyFlags props) const;

    VulkanContext& Ctx_;

    VkImage        Image_      { VK_NULL_HANDLE };
    VkDeviceMemory Memory_     { VK_NULL_HANDLE };
    VkImageView    ImageView_  { VK_NULL_HANDLE };
    VkSampler      Sampler_    { VK_NULL_HANDLE };
    VkDescriptorSet DescSet_   { VK_NULL_HANDLE };
};
