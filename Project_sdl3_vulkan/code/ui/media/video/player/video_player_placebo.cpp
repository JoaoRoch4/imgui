#include "pch.hpp"

#include "video_player_placebo.hpp"

#include "video_player.hpp"
#include "vulkan_context.hpp"

namespace {
uint32_t find_memory_type(VkPhysicalDevice phys,
                          uint32_t filter,
                          VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if ((filter & (1u << i)) && ((mem.memoryTypes[i].propertyFlags & props) == props))
            return i;
    }
    return 0xFFFFFFFFu;
}

bool is_valid_config(const VideoPlayerPlacebo::Config& config) {
    const auto rot = pl_rotation_normalize(PL_ROTATION_0);
    (void)rot;

    // This scaffold currently supports all combinations. Keep validation in one place
    // so future backend constraints are easy to enforce.
    (void)config;
    return true;
}

const std::unordered_set<std::string> k_video_exts = {
    ".mp4", ".mkv", ".avi", ".mov", ".webm", ".flv", ".wmv", ".m4v",
    ".ts",  ".m2ts", ".mpeg", ".mpg", ".ogv",  ".3gp", ".rm", ".rmvb",
    ".divx", ".xvid", ".gif", ".mp3", ".flac", ".ogg", ".wav", ".aac",
    ".opus", ".m4a", ".wma", ".ac3", ".dts", ".tta", ".wv",
};
} // namespace

VideoPlayerPlacebo::VideoPlayerPlacebo()
    : m_player_bridge(std::make_unique<VideoPlayer>()) {
}

VideoPlayerPlacebo::~VideoPlayerPlacebo() {
    shutdown();
}

void VideoPlayerPlacebo::bind_context(vulkan_context *vk) {
    m_vk = vk;
    if (m_player_bridge)
        m_player_bridge->bind_context(vk);
}

void VideoPlayerPlacebo::setup(vulkan_context *vk) {
    setup(vk, kDefaultConfig);
}

void VideoPlayerPlacebo::setup(vulkan_context *vk, Config config) {
    if (!vk)
        return;
    if (!is_valid_config(config))
        return;

    if (m_initialized && m_vk == vk && m_config == config)
        return;

    if (m_initialized)
        shutdown();

    m_vk = vk;
    m_config = config;
    m_initialized = true;

    if (m_player_bridge) {
        m_player_bridge->bind_context(m_vk);
        m_player_bridge->setup(m_vk);
        m_player_bridge->set_all_hwdec(m_config.enable_hwdec);
    }
    if (m_placeholder_descriptor_set == VK_NULL_HANDLE)
        create_placeholder_texture();
}

void VideoPlayerPlacebo::reconfigure(Config config) {
    if (!is_valid_config(config))
        return;

    if (m_config == config)
        return;

    m_config = config;

    if (!m_initialized)
        return;

    if (m_player_bridge)
        m_player_bridge->set_all_hwdec(m_config.enable_hwdec);

    // Placeholder path for future runtime backend reconfiguration.
    // For now, keep the class initialized and only update the cached config.
}

bool VideoPlayerPlacebo::add_from_path(const std::filesystem::path &path) {
    return add_from_path(path, {}, {}, m_config.enable_hwdec, 0, {});
}

bool VideoPlayerPlacebo::add_from_path(const std::filesystem::path &path,
                                       const std::string &title) {
    (void)title;
    return add_from_path(path, title, {}, m_config.enable_hwdec, 0, {});
}

bool VideoPlayerPlacebo::add_from_path(const std::filesystem::path &path,
                                       const std::string &title,
                                       const std::string &logical_source) {
    return add_from_path(path, title, logical_source, m_config.enable_hwdec, 0, {});
}

bool VideoPlayerPlacebo::add_from_path(const std::filesystem::path &path,
                                       const std::string &title,
                                       const std::string &logical_source,
                                       bool hwdec_enabled,
                                       int resume_position_seconds,
                                       const std::string &initial_osd_message) {
    if (!m_initialized && m_vk)
        setup(m_vk, m_config);
    if (!m_initialized)
        return false;

    reconfigure(Config{.enable_hwdec = hwdec_enabled, .prefer_nvdec = hwdec_enabled});
    if (!m_player_bridge)
        return false;

    return m_player_bridge->add_from_path(path,
                                          title,
                                          logical_source,
                                          hwdec_enabled,
                                          resume_position_seconds,
                                          initial_osd_message);
}

bool VideoPlayerPlacebo::add_from_url(const std::string &url,
                                      const std::string &title) {
    return add_from_url(url, title, m_config.enable_hwdec, 0, {});
}

bool VideoPlayerPlacebo::add_from_url(const std::string &url,
                                      const std::string &title,
                                      bool hwdec_enabled,
                                      int resume_position_seconds,
                                      const std::string &initial_osd_message) {
    if (!m_initialized && m_vk)
        setup(m_vk, m_config);
    if (!m_initialized)
        return false;

    reconfigure(Config{.enable_hwdec = hwdec_enabled, .prefer_nvdec = hwdec_enabled});
    if (!m_player_bridge)
        return false;

    return m_player_bridge->add_from_url(url,
                                         title,
                                         hwdec_enabled,
                                         resume_position_seconds,
                                         initial_osd_message);
}

void VideoPlayerPlacebo::update_frames() {
    if (m_player_bridge)
        m_player_bridge->update_frames();
}

bool VideoPlayerPlacebo::create_placeholder_texture() {
    if (!m_vk || !m_initialized)
        return false;
    if (m_placeholder_descriptor_set != VK_NULL_HANDLE)
        return true;

    const int w = m_placeholder_width;
    const int h = m_placeholder_height;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * static_cast<VkDeviceSize>(h) * 4;

    std::vector<uint8_t> pixels(static_cast<size_t>(bytes));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t idx = static_cast<size_t>((y * w + x) * 4);
            const bool checker = (((x / 16) + (y / 16)) & 1) != 0;
            const uint8_t r = static_cast<uint8_t>(checker ? 64 : 28);
            const uint8_t g = static_cast<uint8_t>(checker ? 86 : 36);
            const uint8_t b = static_cast<uint8_t>(checker ? 118 : 48);
            pixels[idx + 0] = r;
            pixels[idx + 1] = g;
            pixels[idx + 2] = b;
            pixels[idx + 3] = 255;
        }
    }

    VkImageCreateInfo image_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1u};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(m_vk->device, &image_info, m_vk->allocator, &m_placeholder_image) != VK_SUCCESS)
        return false;

    VkMemoryRequirements img_req{};
    vkGetImageMemoryRequirements(m_vk->device, m_placeholder_image, &img_req);

    VkMemoryAllocateInfo image_alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    image_alloc.allocationSize = img_req.size;
    image_alloc.memoryTypeIndex = find_memory_type(m_vk->physical_device,
                                                   img_req.memoryTypeBits,
                                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (image_alloc.memoryTypeIndex == 0xFFFFFFFFu)
        return false;
    if (vkAllocateMemory(m_vk->device, &image_alloc, m_vk->allocator, &m_placeholder_image_memory) != VK_SUCCESS)
        return false;
    vkBindImageMemory(m_vk->device, m_placeholder_image, m_placeholder_image_memory, 0);

    VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = m_placeholder_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(m_vk->device, &view_info, m_vk->allocator, &m_placeholder_image_view) != VK_SUCCESS)
        return false;

    VkSamplerCreateInfo sampler_info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 1.0f;
    if (vkCreateSampler(m_vk->device, &sampler_info, m_vk->allocator, &m_placeholder_sampler) != VK_SUCCESS)
        return false;

    m_placeholder_descriptor_set = ImGui_ImplVulkan_AddTexture(
        m_placeholder_sampler,
        m_placeholder_image_view,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    VkBuffer staging_buf = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;

    VkBufferCreateInfo buffer_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = bytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(m_vk->device, &buffer_info, m_vk->allocator, &staging_buf) != VK_SUCCESS)
        return false;

    VkMemoryRequirements buf_req{};
    vkGetBufferMemoryRequirements(m_vk->device, staging_buf, &buf_req);

    VkMemoryAllocateInfo buf_alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    buf_alloc.allocationSize = buf_req.size;
    buf_alloc.memoryTypeIndex = find_memory_type(
        m_vk->physical_device,
        buf_req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (buf_alloc.memoryTypeIndex == 0xFFFFFFFFu)
        return false;
    if (vkAllocateMemory(m_vk->device, &buf_alloc, m_vk->allocator, &staging_mem) != VK_SUCCESS)
        return false;
    vkBindBufferMemory(m_vk->device, staging_buf, staging_mem, 0);

    void *mapped = nullptr;
    vkMapMemory(m_vk->device, staging_mem, 0, bytes, 0, &mapped);
    std::memcpy(mapped, pixels.data(), static_cast<size_t>(bytes));
    vkUnmapMemory(m_vk->device, staging_mem);

    const int frame_idx = static_cast<int>(m_vk->main_window_data.FrameIndex);
    VkCommandPool cmd_pool = m_vk->main_window_data.Frames[frame_idx].CommandPool;

    VkCommandBufferAllocateInfo cmd_alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmd_alloc.commandPool = cmd_pool;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(m_vk->device, &cmd_alloc, &cmd);

    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    VkImageMemoryBarrier b1 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b1.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.srcAccessMask = 0;
    b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b1.image = m_placeholder_image;
    b1.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &b1);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1u};
    vkCmdCopyBufferToImage(cmd,
                           staging_buf,
                           m_placeholder_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1,
                           &region);

    VkImageMemoryBarrier b2 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b2.image = m_placeholder_image;
    b2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0,
                         0,
                         nullptr,
                         0,
                         nullptr,
                         1,
                         &b2);

    vkEndCommandBuffer(cmd);

    VkFenceCreateInfo fence_info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(m_vk->device, &fence_info, m_vk->allocator, &fence);

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    m_vk->queue_submit(1, &submit, fence);
    vkWaitForFences(m_vk->device, 1, &fence, VK_TRUE, UINT64_MAX);

    vkDestroyFence(m_vk->device, fence, m_vk->allocator);
    vkFreeCommandBuffers(m_vk->device, cmd_pool, 1, &cmd);
    vkDestroyBuffer(m_vk->device, staging_buf, m_vk->allocator);
    vkFreeMemory(m_vk->device, staging_mem, m_vk->allocator);

    return true;
}

void VideoPlayerPlacebo::destroy_placeholder_texture() {
    if (!m_vk)
        return;

    if (m_placeholder_descriptor_set != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(m_placeholder_descriptor_set);
        m_placeholder_descriptor_set = VK_NULL_HANDLE;
    }
    if (m_placeholder_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_vk->device, m_placeholder_sampler, m_vk->allocator);
        m_placeholder_sampler = VK_NULL_HANDLE;
    }
    if (m_placeholder_image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_vk->device, m_placeholder_image_view, m_vk->allocator);
        m_placeholder_image_view = VK_NULL_HANDLE;
    }
    if (m_placeholder_image != VK_NULL_HANDLE) {
        vkDestroyImage(m_vk->device, m_placeholder_image, m_vk->allocator);
        m_placeholder_image = VK_NULL_HANDLE;
    }
    if (m_placeholder_image_memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_vk->device, m_placeholder_image_memory, m_vk->allocator);
        m_placeholder_image_memory = VK_NULL_HANDLE;
    }
}

void VideoPlayerPlacebo::draw() {
    if (!m_initialized)
        return;

    if (m_player_bridge && m_player_bridge->has_open_windows()) {
        m_player_bridge->draw();
        return;
    }

    if (m_placeholder_descriptor_set == VK_NULL_HANDLE)
        create_placeholder_texture();

    std::vector<std::string> still_open;
    still_open.reserve(m_open_sources.size());

    for (size_t i = 0; i < m_open_sources.size(); ++i) {
        const std::string &source = m_open_sources[i];

        bool open = true;
        const std::string title = std::string("VPP: ") + source + "###vpp_" + std::to_string(i);
        if (!ImGui::Begin(title.c_str(), &open)) {
            ImGui::End();
            if (open)
                still_open.push_back(source);
            continue;
        }

        const pl_rect2df ref_rect = {0.0f, 0.0f, k_preview_size.x, k_preview_size.y};
        const float base_w = static_cast<float>(pl_rect_w(ref_rect));
        const float base_h = static_cast<float>(pl_rect_h(ref_rect));
        const float base_aspect = base_h > 0.0f ? (base_w / base_h) : (16.0f / 9.0f);

        const pl_rotation rot = pl_rotation_normalize(static_cast<pl_rotation>(i % PL_ROTATION_360));
        const float rotated_aspect = pl_aspect_rotate(base_aspect, rot);

        const float avail_w = std::max(ImGui::GetContentRegionAvail().x, 120.0f);
        const float preview_w = std::min(avail_w, k_preview_size.x * 2.0f);
        const float preview_h = preview_w / std::max(rotated_aspect, 0.001f);

        ImGui::TextUnformatted("VideoPlayerPlacebo (libplacebo scaffold)");
        ImGui::Text("source: %s", source.c_str());
        ImGui::Text("rotation=%d aspect=%.3f", static_cast<int>(rot), rotated_aspect);
        ImGui::Text("hwdec=%s nvdec=%s",
                    m_config.enable_hwdec ? "on" : "off",
                    m_config.prefer_nvdec ? "on" : "off");

        const ImVec2 preview_size(preview_w, preview_h);
        if (m_placeholder_descriptor_set != VK_NULL_HANDLE) {
            ImGui::Image(std::bit_cast<ImTextureID>(m_placeholder_descriptor_set), preview_size);
        } else {
            ImGui::Dummy(preview_size);
        }

        ImGui::End();

        if (open)
            still_open.push_back(source);
    }

    m_open_sources.swap(still_open);
}

void VideoPlayerPlacebo::notify_download_complete(const std::string &url,
                                                  const std::filesystem::path &cached_path) {
    if (m_player_bridge)
        m_player_bridge->notify_download_complete(url, cached_path);

    const std::string cached = cached_path.string();
    for (std::string &src : m_open_sources) {
        if (src == url) {
            src = cached;
            break;
        }
    }
}

void VideoPlayerPlacebo::replace_source_with_saved_file(const std::string &source,
                                                        const std::filesystem::path &saved_path) {
    if (m_player_bridge)
        m_player_bridge->replace_source_with_saved_file(source, saved_path);

    const std::string saved = saved_path.string();
    for (std::string &src : m_open_sources) {
        if (src == source) {
            src = saved;
            break;
        }
    }
}

bool VideoPlayerPlacebo::close_window(const std::string &source) {
    if (m_player_bridge && m_player_bridge->close_window(source))
        return true;

    const auto before = m_open_sources.size();
    std::erase(m_open_sources, source);
    return m_open_sources.size() != before;
}

void VideoPlayerPlacebo::close_all_windows() {
    if (m_player_bridge)
        m_player_bridge->close_all_windows();
    m_open_sources.clear();
}

bool VideoPlayerPlacebo::has_open_windows() const {
    if (m_player_bridge)
        return m_player_bridge->has_open_windows() || !m_open_sources.empty();
    return !m_open_sources.empty();
}

std::vector<std::string> VideoPlayerPlacebo::open_sources() const {
    if (m_player_bridge)
        return m_player_bridge->open_sources();
    return m_open_sources;
}

VkDescriptorSet VideoPlayerPlacebo::get_open_thumbnail(const std::string &source) const {
    if (m_player_bridge)
        return m_player_bridge->get_open_thumbnail(source);
    (void)source;
    return VK_NULL_HANDLE;
}

VkDescriptorSet VideoPlayerPlacebo::hover_thumbnail(const std::string &source) {
    if (m_player_bridge)
        return m_player_bridge->hover_thumbnail(source);
    (void)source;
    return VK_NULL_HANDLE;
}

void VideoPlayerPlacebo::notify_hover(const std::string &source) {
    if (m_player_bridge)
        m_player_bridge->notify_hover(source);
}

bool VideoPlayerPlacebo::save_hover_frame(const std::filesystem::path &path) {
    if (m_player_bridge)
        return m_player_bridge->save_hover_frame(path);
    (void)path;
    return false;
}

bool VideoPlayerPlacebo::is_video_path(const std::filesystem::path &path) {
    return k_video_exts.count(path.extension().string()) > 0;
}

bool VideoPlayerPlacebo::is_video_url(const std::string &url) {
    const auto clean = url.substr(0, url.find('?'));
    const auto ext = std::filesystem::path(clean).extension().string();
    if (k_video_exts.count(ext) > 0)
        return true;
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

void VideoPlayerPlacebo::set_downloader(VideoDownloader *d) {
    m_downloader = d;
    if (m_player_bridge)
        m_player_bridge->set_downloader(d);
}

void VideoPlayerPlacebo::restart_hover_preview() {
    if (m_player_bridge)
        m_player_bridge->restart_hover_preview();
}

bool VideoPlayerPlacebo::consume_hover_popup_reopen_request() {
    if (m_player_bridge)
        return m_player_bridge->consume_hover_popup_reopen_request();
    return false;
}

bool VideoPlayerPlacebo::is_hover_dwell_pending(const std::string &source) const {
    if (m_player_bridge)
        return m_player_bridge->is_hover_dwell_pending(source);
    (void)source;
    return false;
}

bool VideoPlayerPlacebo::can_toggle_hwdec(const std::string &source) const {
    if (m_player_bridge)
        return m_player_bridge->can_toggle_hwdec(source);
    return !source.empty();
}

bool VideoPlayerPlacebo::is_hwdec_enabled(const std::string &source) const {
    if (m_player_bridge)
        return m_player_bridge->is_hwdec_enabled(source);
    (void)source;
    return m_config.enable_hwdec;
}

int VideoPlayerPlacebo::current_position_seconds(const std::string &source) const {
    if (m_player_bridge)
        return m_player_bridge->current_position_seconds(source);
    (void)source;
    return 0;
}

int VideoPlayerPlacebo::persisted_position_seconds(const std::string &source) const {
    if (m_player_bridge)
        return m_player_bridge->persisted_position_seconds(source);
    (void)source;
    return 0;
}

void VideoPlayerPlacebo::set_resume_persist_min_duration_seconds(int seconds) {
    m_resume_persist_min_duration_seconds = std::max(0, seconds);
}

int VideoPlayerPlacebo::resume_persist_min_duration_seconds() const {
    return m_resume_persist_min_duration_seconds;
}

void VideoPlayerPlacebo::toggle_hwdec(const std::string &source) {
    if (m_player_bridge) {
        m_player_bridge->toggle_hwdec(source);
        return;
    }
    (void)source;
    reconfigure(Config{
        .enable_hwdec = !m_config.enable_hwdec,
        .prefer_nvdec = !m_config.enable_hwdec,
    });
}

void VideoPlayerPlacebo::sync_history_state(
    std::vector<WindowStateToml::ImageHistoryEntry> &history) const {
    if (m_player_bridge) {
        m_player_bridge->sync_history_state(history);
        return;
    }

    for (auto &entry : history) {
        if (std::find(m_open_sources.begin(), m_open_sources.end(), entry.source) == m_open_sources.end())
            continue;
        entry.hwdec_enabled = m_config.enable_hwdec;
    }
}

void VideoPlayerPlacebo::set_all_hwdec(bool enabled) {
    reconfigure(Config{.enable_hwdec = enabled, .prefer_nvdec = enabled});
    if (m_player_bridge)
        m_player_bridge->set_all_hwdec(enabled);
}

void VideoPlayerPlacebo::set_all_loop(bool enabled) {
    m_global_loop_enabled = enabled;
    if (m_player_bridge)
        m_player_bridge->set_all_loop(enabled);
}

void VideoPlayerPlacebo::restart_all_threads() {
    if (m_player_bridge)
        m_player_bridge->restart_all_threads();
}

void VideoPlayerPlacebo::set_context_menu(
    VideoContextMenu *ctx,
    std::function<WindowStateToml::ImageHistoryEntry *(const std::string &)> lookup,
    std::function<void(const std::string &)> on_erase) {
    m_ctx_menu = ctx;
    m_ctx_lookup = std::move(lookup);
    m_ctx_on_erase = std::move(on_erase);

    if (m_player_bridge)
        m_player_bridge->set_context_menu(m_ctx_menu, m_ctx_lookup, m_ctx_on_erase);
}

void VideoPlayerPlacebo::set_player_menu_callbacks(
    std::function<void()> on_open_image,
    std::function<void()> on_open_online,
    std::function<void(const std::string &, const std::string &)> on_open_recent,
    std::function<const std::vector<WindowStateToml::ImageHistoryEntry> &()> history,
    HistoryPreview *preview,
    std::function<void(const std::string &)> on_fix_videos,
    std::function<bool(const std::string &)> is_startup_videos_fixed,
    std::function<bool()> on_get_app_fullscreen,
    std::function<void(bool)> on_set_app_fullscreen) {
    m_on_open_image = std::move(on_open_image);
    m_on_open_online = std::move(on_open_online);
    m_on_open_recent = std::move(on_open_recent);
    m_history_provider = std::move(history);
    m_history_preview = preview;
    m_on_fix_videos = std::move(on_fix_videos);
    m_is_startup_videos_fixed = std::move(is_startup_videos_fixed);
    m_on_get_app_fullscreen = std::move(on_get_app_fullscreen);
    m_on_set_app_fullscreen = std::move(on_set_app_fullscreen);

    if (m_player_bridge) {
        m_player_bridge->set_player_menu_callbacks(
            m_on_open_image,
            m_on_open_online,
            m_on_open_recent,
            m_history_provider,
            m_history_preview,
            m_on_fix_videos,
            m_is_startup_videos_fixed,
            m_on_get_app_fullscreen,
            m_on_set_app_fullscreen);
    }
}

void VideoPlayerPlacebo::shutdown() {
    if (!m_initialized && m_vk == nullptr)
        return;

    if (m_player_bridge)
        m_player_bridge->shutdown();

    destroy_placeholder_texture();

    m_initialized = false;
    m_vk = nullptr;
    m_open_sources.clear();
}

bool VideoPlayerPlacebo::initialized() const noexcept {
    return m_initialized;
}

const VideoPlayerPlacebo::Config &VideoPlayerPlacebo::config() const noexcept {
    return m_config;
}

void VideoPlayerPlacebo::reset_to_defaults() {
    reconfigure(kDefaultConfig);
}
