#pragma once

#include "pch.hpp"

class vulkan_context;

class VideoHoverPreview {
public:
    static inline ImVec2 preview_size = {1920, 1080};
    static constexpr size_t max_cache_size = 128;

    /// Runtime-mutable: enable/disable the hover preview popup entirely.
    static inline bool enabled = true;

    /// Runtime-mutable: dwell time before the popup appears and mpv starts loading.
    static inline std::chrono::milliseconds hover_delay{300};

    /// Kill the worker thread when no hover requests arrive for this interval.
    static constexpr std::chrono::milliseconds idle_thread_timeout{100};

    /// If preview loading stays stuck longer than this, restart hover thread.
    static constexpr std::chrono::milliseconds loading_restart_timeout{100};

    /// Prevent rapid restart loops when a source is persistently broken.
    static constexpr std::chrono::milliseconds loading_restart_cooldown{200};

    /// If a hovered/playing source has no decoded frame for too long, force recovery.
    static constexpr std::chrono::milliseconds no_frame_restart_timeout{100};

    VideoHoverPreview();
    ~VideoHoverPreview();

    void setup(vulkan_context *vk);
    void shutdown();

    VkDescriptorSet thumbnail(const std::string &source);
    void notify_hover(const std::string &source);
    bool save_frame(const std::filesystem::path &path);
    void tick_idle();
    bool consume_popup_reopen_request();
    [[nodiscard]] bool is_hover_dwell_pending(const std::string &source) const;

    struct GpuSlot {
        VkImage image{};
        VkDeviceMemory memory{};
        VkImageView view{};
        VkSampler sampler{};
        VkDescriptorSet descriptor{};
        VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
        bool has_frame = false;

        std::list<std::string>::iterator lru_it;
    };

    // core
    void init_mpv();
    void start_thread();
    void stop_thread(bool unregister_watch = true);

    void load_source(const std::string &source);
    void start_playback(const std::string &source);
    void stop_playback();

    void flush_pending_upload();

    // cache
    bool create_slot(const std::string &source);
    void destroy_slot(const std::string &source);
    void touch_lru(const std::string &source);
    void evict_if_needed();

    // vulkan
    bool create_shared();
    void destroy_shared();

private:
    vulkan_context *m_vk{};

    std::unordered_map<std::string, GpuSlot> m_cache;
    std::list<std::string> m_lru;

    std::string m_current;
    std::string m_playing;
    std::chrono::steady_clock::time_point m_last_load_time{};
    std::chrono::steady_clock::time_point m_last_use_time{};
    std::chrono::steady_clock::time_point m_last_restart_time{};
    bool m_has_first_preview_frame{false};
    uint64_t m_watchdog_restart_count{0};
    uint64_t m_idle_stop_count{0};
    std::string m_last_restart_source;

    // ---- Hover-dwell delay --------------------------------------------------
    /// The source string that the cursor is currently resting on.
    /// This may differ from m_current if the dwell period has not elapsed yet.
    std::string m_hovered_source;

    /// Monotonic timestamp of when the cursor first moved onto m_hovered_source.
    /// Reset every time the hovered source changes.
    std::chrono::steady_clock::time_point m_hover_start{};
    // -------------------------------------------------------------------------

    mpv_handle *m_mpv{};
    mpv_render_context *m_render{};

    std::atomic<bool> m_frame_dirty{false};
    std::atomic<bool> m_waiting{false};
    std::atomic<bool> m_upload_pending{false};
    std::atomic<uint64_t> m_thread_watch_id{0};
    std::atomic<bool> m_popup_reopen_requested{false};

    std::jthread m_thread;

    std::vector<uint8_t> m_buf;
    std::mutex m_buf_mutex;
    std::string m_pending_source;

    VkBuffer m_staging{};
    VkDeviceMemory m_staging_mem{};
    void *m_mapped{};

    VkCommandPool m_pool{};
    VkCommandBuffer m_cmd{};
    VkFence m_fence{};

    int m_w{}, m_h{};
};