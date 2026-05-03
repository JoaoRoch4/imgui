#pragma once

#include "pch.hpp"
#include "video_osd_overlay.hpp"
#include "video_seek_preview.hpp"
#include "window_state_toml.hpp"
struct VideoEntry {
        VideoEntry();

        mpv_handle         *mpv;
        mpv_render_context *render_ctx;
        std::atomic<bool>   frame_dirty;

        int                  video_w;
        int                  video_h;
        std::vector<uint8_t> pixel_buf;

        // Vulkan resources (created once VIDEO_RECONFIG fires with valid dims)
        VkImage         image;
        VkDeviceMemory  image_memory;
        VkImageView     image_view;
        VkSampler       sampler;
        VkDescriptorSet descriptor_set;
        VkBuffer        staging_buf;
        VkDeviceMemory  staging_mem;
        void           *staging_mapped;
        VkCommandPool   cmd_pool;
        VkCommandBuffer cmd_buf;
        VkFence         upload_fence;
        bool            upload_in_flight;
        std::chrono::steady_clock::time_point last_upload_time;

        // Entry metadata
        std::string title;
        std::string source;
        std::string playback_source;
        std::string kind; ///< "video", "gif", or "audio"
        int         id;
        bool        open;
        bool        fullscreen;
        bool        loop;
        bool        hwdec_enabled;
        int         resume_position_seconds;
        bool        resume_seek_pending;
        bool        reload_requested;
        bool        load_failed;
        bool        media_unloaded;
        bool        intentional_stop_pending;
        bool        finished_at_eof;   ///< true after EOF — resume position resets to 0
        bool        show_stats;
        bool        hide_ui;
        bool        auto_hide_ui;
        std::string reload_osd_message;

        VideoOsdOverlay osd;

        // Seek-preview thumbnail (dedicated mpv + jthread via VideoSeekPreview)
        VideoSeekPreview seek_preview;
    };