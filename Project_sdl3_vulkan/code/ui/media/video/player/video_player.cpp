#include "video_player.hpp"
#include "video_context_menu.hpp"
#include "history_preview.hpp"
#include "video_downloader.hpp"
#include "image_downloader.hpp"
#include "core/log/debug_log.hpp"


#include <mpv/client.h>
#include <mpv/render.h>

#include <unordered_set>

// ============================================================================
// Extension sets
// ============================================================================

namespace {

const std::unordered_set<std::string> k_video_exts = {
    ".mp4",
    ".mkv",
    ".avi",
    ".mov",
    ".webm",
    ".flv",
    ".wmv",
    ".m4v",
    ".ts",
    ".m2ts",
    ".mpeg",
    ".mpg",
    ".ogv",
    ".3gp",
    ".rm",
    ".rmvb",
    ".divx",
    ".xvid",
    ".gif",
};

const std::unordered_set<std::string> k_audio_exts = {
    ".mp3",
    ".flac",
    ".ogg",
    ".wav",
    ".aac",
    ".opus",
    ".m4a",
    ".wma",
    ".ac3",
    ".dts",
    ".tta",
    ".wv",
};

// Image extensions that should NOT be routed to the video player.
const std::unordered_set<std::string> k_image_exts = {
    ".jpg",
    ".jpeg",
    ".png",
    ".bmp",
    ".tga",
    ".webp",
};

// Hostnames whose URLs mpv/yt-dlp can stream directly.
const std::unordered_set<std::string> k_streaming_hosts = {
    "youtube.com",
    "www.youtube.com",
    "youtu.be",
    "vimeo.com",
    "www.vimeo.com",
    "twitch.tv",
    "www.twitch.tv",
    "dailymotion.com",
    "www.dailymotion.com",
    "reddit.com",
    "www.reddit.com",
    "v.redd.it",
    "twitter.com",
    "www.twitter.com",
    "x.com",
    "tiktok.com",
    "www.tiktok.com",
    "streamable.com",
    "www.streamable.com",
    "nicovideo.jp",
    "www.nicovideo.jp",
    "bilibili.com",
    "www.bilibili.com",
    "soundcloud.com",
    "www.soundcloud.com",
    "bandcamp.com",
};

} // namespace

// ============================================================================
// VideoEntry constructor
// ============================================================================

VideoPlayer::VideoEntry::VideoEntry()
    : mpv{nullptr}
    , render_ctx{nullptr}
    , frame_dirty{false}
    , video_w{0}
    , video_h{0}
    , pixel_buf{}
    , image{VK_NULL_HANDLE}
    , image_memory{VK_NULL_HANDLE}
    , image_view{VK_NULL_HANDLE}
    , sampler{VK_NULL_HANDLE}
    , descriptor_set{VK_NULL_HANDLE}
    , staging_buf{VK_NULL_HANDLE}
    , staging_mem{VK_NULL_HANDLE}
    , staging_mapped{nullptr}
    , cmd_pool{VK_NULL_HANDLE}
    , cmd_buf{VK_NULL_HANDLE}
    , upload_fence{VK_NULL_HANDLE}
    , upload_in_flight{false}
    , last_upload_time{}
    , title{}
    , source{}
    , kind{}
    , id{0}
    , open{true}
    , fullscreen{false}
    , loop{false}
    , hwdec_enabled{false}
    , resume_position_seconds{0}
    , resume_seek_pending{false}
    , reload_requested{false}
    , load_failed{false}
    , show_stats{false}
    , reload_osd_message{}
    , osd{}
{
}

// ============================================================================
// VideoPlayer constructor / destructor
// ============================================================================

VideoPlayer::VideoPlayer()
    : m_hover{}
    , m_seek_uploader{}
    , m_ui_window{}
    , m_vk{nullptr}
    , m_entries{}
    , m_next_id{0}
    , m_resume_persist_min_duration_seconds{
        WindowStateToml{}.video_resume_persist_min_duration_seconds}
    , m_ctx_menu{nullptr}
    , m_ctx_lookup{}
    , m_ctx_on_erase{}
    , m_on_open_image{}
    , m_on_open_online{}
    , m_on_open_recent{}
    , m_history_provider{}
    , m_history_preview{nullptr}
    , m_on_fix_videos{}
    , m_is_startup_videos_fixed{}
    , m_downloader{nullptr}
{
}

VideoPlayer::~VideoPlayer() {
    if (m_vk)
        shutdown();
}

// ============================================================================
// Lifecycle
// ============================================================================

void VideoPlayer::setup(vulkan_context *vk) {
    APP_DEBUG_LOG("[VideoPlayer] setup");
    m_vk = vk;
    constexpr VkDeviceSize k_max_seek_preview_bytes =
        static_cast<VkDeviceSize>(1920) * 1920 * 4;
    m_seek_uploader.init(m_vk,
                         static_cast<size_t>(k_max_seek_preview_bytes));
    m_hover.setup(vk);
}

void VideoPlayer::shutdown() {
    APP_DEBUG_LOG("[VideoPlayer] shutdown begin");
    if (!m_vk)
        return;

    // Stop all background threads before vkDeviceWaitIdle
    m_hover.stop_thread();
    for (auto &ep : m_entries)
        ep->seek_preview.stop_thread();

    vkDeviceWaitIdle(m_vk->device);

    for (auto &ep : m_entries) {
        if (ep->render_ctx) {
            mpv_render_context_free(ep->render_ctx);
            ep->render_ctx = nullptr;
        }
        destroy_gpu_resources(*ep);
        if (ep->mpv) {
            mpv_terminate_destroy(ep->mpv);
            ep->mpv = nullptr;
        }
        ep->seek_preview.shutdown();
    }
    m_entries.clear();

    m_hover.shutdown();
    m_seek_uploader.shutdown();

    m_vk = nullptr;
    APP_DEBUG_LOG("[VideoPlayer] shutdown done");
}

// ============================================================================
// Static helpers
// ============================================================================

bool VideoPlayer::is_video_path(const std::filesystem::path &path) {
    const auto ext = path.extension().string();
    return k_video_exts.count(ext) > 0 || k_audio_exts.count(ext) > 0;
}

bool VideoPlayer::is_video_url(const std::string &url) {
    const bool has_scheme = url.find("://") != std::string::npos;

    // 1. Known streaming hostnames — always route to mpv regardless of extension
    const auto host_start = url.find("://");
    if (host_start != std::string::npos) {
        const auto path_start = url.find('/', host_start + 3);
        const std::string host = url.substr(
            host_start + 3,
            path_start == std::string::npos ? std::string::npos : path_start - (host_start + 3));
        if (k_streaming_hosts.count(host) > 0)
            return true;
    }

    // 2. Check extension (strip query string first)
    const auto clean = url.substr(0, url.find('?'));
    const auto ext = std::filesystem::path(clean).extension().string();
    if (has_scheme && (k_video_exts.count(ext) > 0 || k_audio_exts.count(ext) > 0))
        return true;

    // 3. Any http/https URL that doesn't look like a still image → let mpv try
    const bool is_http = url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
    if (is_http && k_image_exts.count(ext) == 0 && ext.empty())
        return true;

    return false;
}

uint32_t VideoPlayer::find_memory_type(VkPhysicalDevice phys,
                                       uint32_t filter,
                                       VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
        if ((filter & (1u << i)) && ((mem.memoryTypes[i].propertyFlags & props) == props))
            return i;
    return 0xFFFFFFFFu;
}

// ============================================================================
// Add entries
// ============================================================================

bool VideoPlayer::add_from_path(const std::filesystem::path &path) {
    return add_from_path(path, {}, {}, false, 0, {});
}

bool VideoPlayer::add_from_path(const std::filesystem::path &path,
                                const std::string &title) {
    return add_from_path(path, title, {}, false, 0, {});
}

bool VideoPlayer::add_from_path(const std::filesystem::path &path,
                                const std::string &title,
                                const std::string &logical_source) {
    return add_from_path(path, title, logical_source, false, 0, {});
}

bool VideoPlayer::add_from_path(const std::filesystem::path &path,
                                const std::string &title,
                                const std::string &logical_source,
                                bool hwdec_enabled,
                                int resume_position_seconds,
                                const std::string &initial_osd_message) {
    APP_DEBUG_LOG("[VideoPlayer] add_from_path: {}", path.string());
    auto ep = std::make_unique<VideoEntry>();
    ep->title = title.empty() ? path.filename().string() : title;
    ep->source = logical_source.empty() ? path.string() : logical_source;
    ep->playback_source = path.string();
    ep->kind = (path.extension().string() == ".gif") ? "gif" : "video";
    ep->id = m_next_id++;
    ep->hwdec_enabled = hwdec_enabled;
    ep->resume_position_seconds = std::max(resume_position_seconds, 0);
    ep->resume_seek_pending = ep->resume_position_seconds > 0;

    ep->mpv = mpv_create();
    if (!ep->mpv)
        return false;

    mpv_set_option_string(ep->mpv, "hwdec", ep->hwdec_enabled ? "auto-safe" : "no");
    mpv_set_option_string(ep->mpv, "vo", "libmpv");
    if (ep->kind == "gif") {
        mpv_set_option_string(ep->mpv, "loop-file", "inf");
        ep->loop = true;
    }

    if (mpv_initialize(ep->mpv) < 0) {
        mpv_terminate_destroy(ep->mpv);
        ep->mpv = nullptr;
        return false;
    }

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW)},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_create(&ep->render_ctx, ep->mpv, params) < 0) {
        mpv_terminate_destroy(ep->mpv);
        ep->mpv = nullptr;
        return false;
    }

    VideoEntry *raw = ep.get();
    mpv_render_context_set_update_callback(
        ep->render_ctx,
        [](void *ctx) {
            static_cast<VideoEntry *>(ctx)->frame_dirty.store(
                true, std::memory_order_release);
        },
        raw);

    const char *cmd[] = {"loadfile", ep->playback_source.c_str(), nullptr};
    mpv_command_async(ep->mpv, 0, cmd);
    {
        int start_paused = 1;
        mpv_set_property(ep->mpv, "pause", MPV_FORMAT_FLAG, &start_paused);
    }
    {
        int start_paused = 1;
        mpv_set_property(ep->mpv, "pause", MPV_FORMAT_FLAG, &start_paused);
    }

    ep->seek_preview.setup(m_vk, &m_seek_uploader, ep->playback_source);
    if (!initial_osd_message.empty())
        ep->osd.show(initial_osd_message);

    m_entries.push_back(std::move(ep));
    return true;
}

bool VideoPlayer::add_from_url(const std::string &url, const std::string &title) {
    return add_from_url(url, title, false, 0, {});
}

bool VideoPlayer::add_from_url(const std::string &url,
                               const std::string &title,
                               bool hwdec_enabled,
                               int resume_position_seconds,
                               const std::string &initial_osd_message) {
    APP_DEBUG_LOG("[VideoPlayer] add_from_url: {} (title='{}')", url, title);
    auto ep = std::make_unique<VideoEntry>();
    ep->title = title;
    ep->source = url;
    ep->playback_source = url;
    ep->kind = "video";
    ep->id = m_next_id++;
    ep->hwdec_enabled = hwdec_enabled;
    ep->resume_position_seconds = std::max(resume_position_seconds, 0);
    ep->resume_seek_pending = ep->resume_position_seconds > 0;

    ep->mpv = mpv_create();
    if (!ep->mpv)
        return false;

    mpv_set_option_string(ep->mpv, "hwdec", ep->hwdec_enabled ? "auto-safe" : "no");
    mpv_set_option_string(ep->mpv, "vo", "libmpv");
    // yt-dlp integration — lets mpv stream YouTube, Vimeo, Twitch, etc.
    mpv_set_option_string(ep->mpv, "ytdl", "yes");
    mpv_set_option_string(ep->mpv, "ytdl-format",
                          "bestvideo[height<=1080]+bestaudio/best[height<=1080]/best");
    // Network buffering
    mpv_set_option_string(ep->mpv, "cache", "yes");
    mpv_set_option_string(ep->mpv, "demuxer-max-bytes", "150MiB");
    mpv_set_option_string(ep->mpv, "demuxer-max-back-bytes", "50MiB");
    mpv_set_option_string(ep->mpv, "demuxer-readahead-secs", "30");
    mpv_set_option_string(ep->mpv, "stream-buffer-size", "4MiB");

    if (mpv_initialize(ep->mpv) < 0) {
        mpv_terminate_destroy(ep->mpv);
        ep->mpv = nullptr;
        return false;
    }

    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW)},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_create(&ep->render_ctx, ep->mpv, params) < 0) {
        mpv_terminate_destroy(ep->mpv);
        ep->mpv = nullptr;
        return false;
    }

    VideoEntry *raw = ep.get();
    mpv_render_context_set_update_callback(
        ep->render_ctx,
        [](void *ctx) {
            static_cast<VideoEntry *>(ctx)->frame_dirty.store(
                true, std::memory_order_release);
        },
        raw);

    const char *cmd[] = {"loadfile", url.c_str(), nullptr};
    mpv_command_async(ep->mpv, 0, cmd);

    // Seek preview requires a locally cached file; skip for remote URLs.
    if (!initial_osd_message.empty())
        ep->osd.show(initial_osd_message);

    m_entries.push_back(std::move(ep));
    return true;
}

// ============================================================================
// GPU resource management
// ============================================================================

bool VideoPlayer::create_gpu_resources(VideoEntry &e) {
    APP_DEBUG_LOG("[VideoPlayer] create_gpu_resources id={} {}x{}", e.id, e.video_w, e.video_h);
    const int w = e.video_w;
    const int h = e.video_h;
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;

    // -- VkImage (device-local, OPTIMAL tiling) ------------------------------
    {
        VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1u};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(m_vk->device, &info, m_vk->allocator, &e.image) != VK_SUCCESS)
            return false;

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(m_vk->device, e.image, &req);

        VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = find_memory_type(m_vk->physical_device,
                                                 req.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (vkAllocateMemory(m_vk->device, &alloc, m_vk->allocator, &e.image_memory) != VK_SUCCESS)
            return false;

        vkBindImageMemory(m_vk->device, e.image, e.image_memory, 0);
    }

    // -- VkImageView ---------------------------------------------------------
    {
        VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        info.image = e.image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_vk->device, &info, m_vk->allocator, &e.image_view);
    }

    // -- VkSampler -----------------------------------------------------------
    {
        VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        vkCreateSampler(m_vk->device, &info, m_vk->allocator, &e.sampler);
    }

    // -- ImGui descriptor (registers with ImGui_ImplVulkan) ------------------
    e.descriptor_set = ImGui_ImplVulkan_AddTexture(
        e.sampler, e.image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // -- Staging buffer (HOST_VISIBLE, persistently mapped) ------------------
    {
        VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        vkCreateBuffer(m_vk->device, &info, m_vk->allocator, &e.staging_buf);

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(m_vk->device, e.staging_buf, &req);

        VkMemoryAllocateInfo alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = find_memory_type(
            m_vk->physical_device, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        vkAllocateMemory(m_vk->device, &alloc, m_vk->allocator, &e.staging_mem);
        vkBindBufferMemory(m_vk->device, e.staging_buf, e.staging_mem, 0);
        vkMapMemory(m_vk->device, e.staging_mem, 0, bytes, 0, &e.staging_mapped);
    }

    // -- Dedicated command pool (resettable) ---------------------------------
    {
        VkCommandPoolCreateInfo info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        info.queueFamilyIndex = m_vk->queue_family;
        info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        vkCreateCommandPool(m_vk->device, &info, m_vk->allocator, &e.cmd_pool);

        VkCommandBufferAllocateInfo alloc = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool = e.cmd_pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        vkAllocateCommandBuffers(m_vk->device, &alloc, &e.cmd_buf);
    }

    // -- Reusable fence for upload synchronisation ---------------------------
    {
        VkFenceCreateInfo info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        vkCreateFence(m_vk->device, &info, m_vk->allocator, &e.upload_fence);
    }

    // -- CPU pixel buffer ----------------------------------------------------
    e.pixel_buf.resize(static_cast<size_t>(w) * h * 4);

    // -- Transition image to SHADER_READ_ONLY so it can be sampled immediately
    {
        VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(e.cmd_buf, &begin);

        VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.image = e.image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(e.cmd_buf,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);

        vkEndCommandBuffer(e.cmd_buf);

        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &e.cmd_buf;
        m_vk->queue_submit(1, &submit, e.upload_fence);
        vkWaitForFences(m_vk->device, 1, &e.upload_fence, VK_TRUE, UINT64_MAX);
        vkResetFences(m_vk->device, 1, &e.upload_fence);
        vkResetCommandBuffer(e.cmd_buf, 0);
    }

    return true;
}

void VideoPlayer::destroy_gpu_resources(VideoEntry &e) {
    APP_DEBUG_LOG("[VideoPlayer] destroy_gpu_resources id={}", e.id);
    e.upload_in_flight = false;
    e.last_upload_time = {};
    if (e.descriptor_set != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(e.descriptor_set);
        e.descriptor_set = VK_NULL_HANDLE;
    }
    if (e.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_vk->device, e.sampler, m_vk->allocator);
        e.sampler = VK_NULL_HANDLE;
    }
    if (e.image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(m_vk->device, e.image_view, m_vk->allocator);
        e.image_view = VK_NULL_HANDLE;
    }
    if (e.image != VK_NULL_HANDLE) {
        vkDestroyImage(m_vk->device, e.image, m_vk->allocator);
        e.image = VK_NULL_HANDLE;
    }
    if (e.image_memory != VK_NULL_HANDLE) {
        vkFreeMemory(m_vk->device, e.image_memory, m_vk->allocator);
        e.image_memory = VK_NULL_HANDLE;
    }
    if (e.upload_fence != VK_NULL_HANDLE) {
        vkDestroyFence(m_vk->device, e.upload_fence, m_vk->allocator);
        e.upload_fence = VK_NULL_HANDLE;
    }
    // Destroying the pool also frees the command buffer
    if (e.cmd_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_vk->device, e.cmd_pool, m_vk->allocator);
        e.cmd_pool = VK_NULL_HANDLE;
        e.cmd_buf = VK_NULL_HANDLE;
    }
    if (e.staging_buf != VK_NULL_HANDLE) {
        if (e.staging_mapped) {
            vkUnmapMemory(m_vk->device, e.staging_mem);
            e.staging_mapped = nullptr;
        }
        vkDestroyBuffer(m_vk->device, e.staging_buf, m_vk->allocator);
        e.staging_buf = VK_NULL_HANDLE;
    }
    if (e.staging_mem != VK_NULL_HANDLE) {
        vkFreeMemory(m_vk->device, e.staging_mem, m_vk->allocator);
        e.staging_mem = VK_NULL_HANDLE;
    }
}

// ============================================================================
// Hover thumbnail / seek thumbnail — delegated to child classes
// ============================================================================

VkDescriptorSet VideoPlayer::hover_thumbnail(const std::string &source) {
    static std::string s_last_hover_source;
    if (s_last_hover_source != source) {
        APP_DEBUG_LOG("[VideoPlayer] hover_thumbnail: {}", source);
        s_last_hover_source = source;
    }
    return m_hover.thumbnail(source);
}

bool VideoPlayer::save_hover_frame(const std::filesystem::path &path) {
    APP_DEBUG_LOG("[VideoPlayer] save_hover_frame: {}", path.string());
    return m_hover.save_frame(path);
}

VkDescriptorSet VideoPlayer::get_open_thumbnail(const std::string &source) const {
    static std::string s_last_hit_source;
    for (const auto &ep : m_entries) {
        if (ep->source == source && ep->descriptor_set != VK_NULL_HANDLE) {
            if (s_last_hit_source != source) {
                APP_DEBUG_LOG("[VideoPlayer] get_open_thumbnail: HIT {}", source);
                s_last_hit_source = source;
            }
            return ep->descriptor_set;
        }
        if (ep->playback_source == source && ep->descriptor_set != VK_NULL_HANDLE)
            return ep->descriptor_set;
    }
    return VK_NULL_HANDLE;
}

// ============================================================================
// Per-frame upload
// ============================================================================

bool VideoPlayer::upload_frame(VideoEntry &e) {
    if (!e.staging_mapped || e.video_w <= 0 || e.video_h <= 0)
        return false;

    // Never block the UI thread waiting on a prior upload.
    if (e.upload_in_flight) {
        const VkResult fence_status = vkGetFenceStatus(m_vk->device, e.upload_fence);
        if (fence_status == VK_NOT_READY)
            return false;
        if (fence_status == VK_SUCCESS) {
            vkResetFences(m_vk->device, 1, &e.upload_fence);
            vkResetCommandBuffer(e.cmd_buf, 0);
            e.upload_in_flight = false;
        }
    }

    const int w = e.video_w;
    const int h = e.video_h;
    size_t stride = static_cast<size_t>(w) * 4;

    // Ask libmpv to render the current video frame into the CPU buffer
    int size_arr[2] = {w, h};
    const char *fmt_rgba = "rgba";
    mpv_render_param render_params[] = {
        {MPV_RENDER_PARAM_SW_SIZE, size_arr},
        {MPV_RENDER_PARAM_SW_FORMAT, const_cast<char *>(fmt_rgba)},
        {MPV_RENDER_PARAM_SW_STRIDE, &stride},
        {MPV_RENDER_PARAM_SW_POINTER, e.pixel_buf.data()},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (mpv_render_context_render(e.render_ctx, render_params) < 0)
        return false;

    // Copy CPU buffer into the persistently-mapped staging buffer
    std::memcpy(e.staging_mapped, e.pixel_buf.data(), e.pixel_buf.size());

    // Record a one-time command buffer: barrier + copy + barrier
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(e.cmd_buf, &begin);

    // SHADER_READ → TRANSFER_DST
    VkImageMemoryBarrier b1 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b1.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b1.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1.image = e.image;
    b1.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b1.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b1.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(e.cmd_buf,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b1);

    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1u};
    vkCmdCopyBufferToImage(e.cmd_buf, e.staging_buf, e.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // TRANSFER_DST → SHADER_READ
    VkImageMemoryBarrier b2 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b2.image = e.image;
    b2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(e.cmd_buf,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b2);

    vkEndCommandBuffer(e.cmd_buf);

    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &e.cmd_buf;
    m_vk->queue_submit(1, &submit, e.upload_fence);
    e.upload_in_flight = true;
    return true;
}

// ============================================================================
// Event polling
// ============================================================================

void VideoPlayer::poll_events(VideoEntry &e) {
    while (true) {
        mpv_event *ev = mpv_wait_event(e.mpv, 0.0);
        if (!ev || ev->event_id == MPV_EVENT_NONE)
            break;

        if (ev->event_id == MPV_EVENT_END_FILE) {
            const auto *edata = static_cast<const mpv_event_end_file *>(ev->data);
            if (edata->reason != MPV_END_FILE_REASON_EOF) {
                APP_DEBUG_LOG("[VideoPlayer] load failed id={} reason={}", e.id, static_cast<int>(edata->reason));
                e.load_failed = true;
            }
        }

        if (ev->event_id == MPV_EVENT_FILE_LOADED && e.resume_seek_pending && e.resume_position_seconds > 0) {
            double resume_position = static_cast<double>(e.resume_position_seconds);
            mpv_set_property(e.mpv, "time-pos", MPV_FORMAT_DOUBLE, &resume_position);
            e.resume_seek_pending = false;
        }

        if (ev->event_id == MPV_EVENT_VIDEO_RECONFIG) {
            APP_DEBUG_LOG("[VideoPlayer] event VIDEO_RECONFIG id={}", e.id);
            // Read the actual decoded video dimensions
            int64_t nw = 0, nh = 0;
            mpv_get_property(e.mpv, "dwidth", MPV_FORMAT_INT64, &nw);
            mpv_get_property(e.mpv, "dheight", MPV_FORMAT_INT64, &nh);

            if (nw > 0 && nh > 0 &&
                (static_cast<int>(nw) != e.video_w ||
                 static_cast<int>(nh) != e.video_h)) {
                vkDeviceWaitIdle(m_vk->device);
                destroy_gpu_resources(e);
                e.video_w = static_cast<int>(nw);
                e.video_h = static_cast<int>(nh);
                create_gpu_resources(e);
                e.frame_dirty.store(true, std::memory_order_release);
                APP_DEBUG_LOG("[VideoPlayer] reconfigured id={} {}x{}", e.id, e.video_w, e.video_h);
            }
        }
    }
}

// ============================================================================
// update_frames — call once per frame
// ============================================================================

void VideoPlayer::update_frames() {
    if (!m_vk)
        return;

    constexpr auto k_min_upload_interval = std::chrono::milliseconds(50);

    for (auto &ep : m_entries) {
        VideoEntry &e = *ep;
        if (!e.mpv)
            continue;

        poll_events(e);

        // If the render-update callback fired, render and upload a new frame.
        // Cap upload cadence so video decode work does not dominate UI pacing.
        if (e.frame_dirty.exchange(false, std::memory_order_acq_rel) &&
            e.descriptor_set != VK_NULL_HANDLE) {
            const auto now = std::chrono::steady_clock::now();
            if ((now - e.last_upload_time) >= k_min_upload_interval) {
                if (upload_frame(e))
                    e.last_upload_time = now;
                else
                    e.frame_dirty.store(true, std::memory_order_release);
            } else {
                e.frame_dirty.store(true, std::memory_order_release);
            }
        }

        // Upload preview thumbnail if the jthread has a new frame ready
        e.seek_preview.update();
    }
}

// ============================================================================
// draw — called each frame inside an ImGui frame
// ============================================================================

void VideoPlayer::set_player_menu_callbacks(
    std::function<void()> on_open_image,
    std::function<void()> on_open_online,
    std::function<void(const std::string &, const std::string &)> on_open_recent,
    std::function<const std::vector<WindowStateToml::ImageHistoryEntry> &()> history,
    HistoryPreview *preview,
    std::function<void(const std::string &)> on_fix_videos,
    std::function<bool(const std::string &)> is_startup_videos_fixed)
{
    m_on_open_image    = std::move(on_open_image);
    m_on_open_online   = std::move(on_open_online);
    m_on_open_recent   = std::move(on_open_recent);
    m_history_provider = std::move(history);
    m_history_preview  = preview;
    m_on_fix_videos    = std::move(on_fix_videos);
    m_is_startup_videos_fixed = std::move(is_startup_videos_fixed);
}

void VideoPlayer::set_context_menu(
    VideoContextMenu *ctx,
    std::function<WindowStateToml::ImageHistoryEntry *(const std::string &)> lookup,
    std::function<void(const std::string &)> on_erase)
{
    m_ctx_menu     = ctx;
    m_ctx_lookup   = std::move(lookup);
    m_ctx_on_erase = std::move(on_erase);
}

void VideoPlayer::set_downloader(VideoDownloader *d)
{
    m_downloader = d;
}

void VideoPlayer::restart_hover_preview()
{
    APP_DEBUG_LOG("[VideoPlayer] restart_hover_preview");
    m_hover.stop_thread();
    m_hover.start_thread();
}

bool VideoPlayer::can_toggle_hwdec(const std::string &source) const
{
    return std::any_of(m_entries.begin(), m_entries.end(), [&source](const std::unique_ptr<VideoEntry> &entry) {
        return entry->open && entry->source == source;
    });
}

bool VideoPlayer::is_hwdec_enabled(const std::string &source) const
{
    for (const auto &entry : m_entries) {
        if (entry->open && entry->source == source)
            return entry->hwdec_enabled;
    }
    return false;
}

int VideoPlayer::current_position_seconds(const std::string &source) const
{
    for (const auto &entry : m_entries) {
        if (!entry->open || entry->source != source)
            continue;

        return current_position_seconds(*entry);
    }

    return 0;
}

int VideoPlayer::persisted_position_seconds(const std::string &source) const
{
    for (const auto &entry : m_entries) {
        if (!entry->open || entry->source != source)
            continue;

        return persisted_position_seconds(*entry);
    }

    return 0;
}

int VideoPlayer::current_position_seconds(const VideoEntry &entry) const
{
    double time_pos = 0.0;
    if (mpv_get_property(entry.mpv, "time-pos", MPV_FORMAT_DOUBLE, &time_pos) >= 0)
        return std::max(static_cast<int>(time_pos), 0);
    return entry.resume_position_seconds;
}

int VideoPlayer::persisted_position_seconds(const VideoEntry &entry) const
{
    double duration = 0.0;
    if (mpv_get_property(entry.mpv, "duration", MPV_FORMAT_DOUBLE, &duration) < 0)
        return current_position_seconds(entry);

    if (duration < static_cast<double>(m_resume_persist_min_duration_seconds))
        return 0;

    return current_position_seconds(entry);
}

void VideoPlayer::set_resume_persist_min_duration_seconds(int seconds)
{
    m_resume_persist_min_duration_seconds = std::max(seconds, 0);
}

int VideoPlayer::resume_persist_min_duration_seconds() const
{
    return m_resume_persist_min_duration_seconds;
}

void VideoPlayer::toggle_hwdec(const std::string &source)
{
    for (auto &entry : m_entries) {
        if (!entry->open || entry->source != source)
            continue;

        entry->hwdec_enabled = !entry->hwdec_enabled;
        entry->resume_position_seconds = current_position_seconds(source);
        entry->resume_seek_pending = entry->resume_position_seconds > 0;
        entry->reload_osd_message = entry->hwdec_enabled ? "HW Decode On" : "HW Decode Off";
        entry->reload_requested = true;
        return;
    }
}

void VideoPlayer::sync_history_state(std::vector<WindowStateToml::ImageHistoryEntry> &history) const
{
    for (auto &history_entry : history) {
        for (const auto &entry : m_entries) {
            if (!entry->open || entry->source != history_entry.source)
                continue;

            history_entry.hwdec_enabled = entry->hwdec_enabled;
            history_entry.resume_position_seconds = persisted_position_seconds(*entry);
            break;
        }
    }
}

void VideoPlayer::notify_download_complete(const std::string &url,
                                           const std::filesystem::path &cached_path)
{
    for (auto &ep : m_entries) {
        if (ep->source == url && ep->open) {
            APP_DEBUG_LOG("[VideoPlayer] download complete for id={} -> {}",
                         ep->id, cached_path.string());
            ep->seek_preview.setup(m_vk, &m_seek_uploader, cached_path.string());
            ep->playback_source = cached_path.string();
            break;
        }
    }
}

void VideoPlayer::replace_source_with_saved_file(const std::string &source,
                                                 const std::filesystem::path &saved_path)
{
    const std::string saved_source = saved_path.string();

    for (auto &ep : m_entries) {
        if (ep->source != source || !ep->open)
            continue;

        ep->source = saved_source;
        ep->playback_source = saved_source;
        ep->title = saved_path.filename().string();
        ep->kind = (saved_path.extension().string() == ".gif") ? "gif" : "video";
        break;
    }
}

void VideoPlayer::draw() {
    if (!m_vk)
        return;

    struct ReloadRequest {
        std::string source;
        std::string title;
        bool        hwdec_enabled;
        int         resume_position_seconds;
        std::string initial_osd_message;
    };

    std::vector<ReloadRequest> reload_queue;

    // First process entries closed in a previous frame. This avoids destroying
    // descriptor sets that may still be referenced by current-frame ImGui draw lists.
    for (auto &ep : m_entries) {
        if (!ep->open && ep->reload_requested) {
            reload_queue.push_back({ep->source,
                                    ep->title,
                                    ep->hwdec_enabled,
                                    ep->resume_position_seconds,
                                    ep->reload_osd_message});
            ep->reload_requested = false;
            ep->load_failed      = false;
            ep->reload_osd_message.clear();
        }
    }

    bool any_closed = std::any_of(m_entries.begin(), m_entries.end(),
                                  [](const std::unique_ptr<VideoEntry> &ep) {
                                      return !ep->open;
                                  });
    if (any_closed)
        vkDeviceWaitIdle(m_vk->device);

    std::erase_if(m_entries, [this](const std::unique_ptr<VideoEntry> &ep) {
        if (ep->open)
            return false;
        // Stop threads before freeing resources they use
        ep->seek_preview.stop_thread();
        if (ep->render_ctx) {
            mpv_render_context_free(ep->render_ctx);
            ep->render_ctx = nullptr;
        }
        destroy_gpu_resources(*ep);
        if (ep->mpv) {
            mpv_terminate_destroy(ep->mpv);
            ep->mpv = nullptr;
        }
        ep->seek_preview.shutdown();
        return true;
    });

    // Re-open entries that requested a reload after old resources were torn down.
    for (const auto &request : reload_queue) {
        const std::string &source = request.source;
        const std::string &title = request.title;
        bool history_cache_exists = false;
        std::string history_cached_path;
        std::error_code cache_ec;
        if (m_ctx_lookup) {
            if (auto *entry = m_ctx_lookup(source)) {
                history_cached_path = entry->cached_path;
                if (!entry->cached_path.empty()) {
                    history_cache_exists = std::filesystem::exists(
                        std::filesystem::path(entry->cached_path), cache_ec);
                }
            }
        }

        std::filesystem::path reload_path;
        bool reopen_as_url = false;

        if (history_cache_exists) {
            reload_path = std::filesystem::path(history_cached_path);
        } else {
            std::error_code source_ec;
            const std::filesystem::path source_path(source);
            if (std::filesystem::exists(source_path, source_ec)) {
                reload_path = source_path;
            } else {
                reopen_as_url = is_video_url(source);
            }
        }

        APP_DEBUG_LOG(
            "[VideoPlayer] executing reload source={} reopen_as_url={} reload_path='{}' history_cached_path='{}' cache_exists={} cache_ec={}",
            source,
            reopen_as_url,
            reload_path.string(),
            history_cached_path,
            history_cache_exists,
            cache_ec ? cache_ec.message() : "ok");

        if (reopen_as_url)
            add_from_url(source,
                         title,
                         request.hwdec_enabled,
                         request.resume_position_seconds,
                         request.initial_osd_message);
        else
            add_from_path(reload_path,
                          title,
                          source,
                          request.hwdec_enabled,
                          request.resume_position_seconds,
                          request.initial_osd_message);
    }

    for (int i = 0; i < static_cast<int>(m_entries.size()); ++i) {
        if (!m_entries[i]->open)
            continue;
        draw_window(*m_entries[i], i);
        if (m_entries[i]->reload_requested) {
            m_entries[i]->open = false;
        }
    }
}

bool VideoPlayer::close_window(const std::string &source) {
    for (auto &ep : m_entries) {
        if (ep->open && ep->source == source) {
            ep->open = false;
            return true;
        }
    }
    return false;
}

void VideoPlayer::close_all_windows() {
    for (auto &ep : m_entries)
        ep->open = false;
}

void VideoPlayer::draw_window(VideoEntry &e, int idx) {
    std::string display_title = e.title;
    if (m_downloader) {
        const uint64_t downloaded = m_downloader->bytes_inflight(e.source);
        if (downloaded > 0) {
            constexpr double k_mb = 1024.0 * 1024.0;
            char buf[32];
            std::snprintf(buf, sizeof(buf), " (%.1f MB↓)", static_cast<double>(downloaded) / k_mb);
            display_title += buf;
        }
    }
    VideoUiWindow::State state{
        e.mpv,
        display_title,
        e.title,
        e.source,
        e.playback_source,
        e.kind,
        e.id,
        e.open,
        e.fullscreen,
        e.loop,
        e.reload_requested,
        e.load_failed,
        e.video_w,
        e.video_h,
        e.descriptor_set,
        e.osd,
        e.seek_preview,
        idx > 0,
        idx < static_cast<int>(m_entries.size()) - 1,
        e.show_stats,
    };

    if (!e.resume_seek_pending) {
        double current_time_pos = 0.0;
        if (mpv_get_property(e.mpv, "time-pos", MPV_FORMAT_DOUBLE, &current_time_pos) >= 0)
            e.resume_position_seconds = std::max(static_cast<int>(current_time_pos), 0);
    }

    VideoUiWindow::Callbacks callbacks{
        m_ctx_menu,
        m_ctx_lookup,
        m_ctx_on_erase,
        [this]() { restart_hover_preview(); },
        [this]() { close_all_windows(); },
        m_on_open_image,
        m_on_open_online,
        m_on_open_recent,
        m_history_provider,
        m_history_preview,
        m_on_fix_videos,
        m_is_startup_videos_fixed,
        [this, &e, idx](int direction) {
            const int target = idx + direction;
            const std::string &source = m_entries[target]->source;
            const std::string &playback_source = m_entries[target]->playback_source;
            const char *command[] = {"loadfile", playback_source.c_str(), "replace", nullptr};
            mpv_command_async(e.mpv, 0, command);
            e.source = source;
            e.playback_source = playback_source;
            e.title = m_entries[target]->title;
            e.kind = m_entries[target]->kind;
        },
    };

    m_ui_window.draw(state, callbacks);
}

// ============================================================================
// Query
// ============================================================================

bool VideoPlayer::has_open_windows() const {
    return std::any_of(m_entries.begin(), m_entries.end(),
                       [](const std::unique_ptr<VideoEntry> &ep) { return ep->open; });
}

std::vector<std::string> VideoPlayer::open_sources() const {
    std::vector<std::string> result;
    for (const auto &ep : m_entries) {
        if (ep->open)
            result.push_back(ep->source);
    }
    return result;
}
