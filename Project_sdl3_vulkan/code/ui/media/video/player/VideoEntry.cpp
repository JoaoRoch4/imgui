#include "VideoEntry.hpp"
#include "pch.hpp"

// ============================================================================
// VideoEntry constructor
// ============================================================================
VideoEntry::VideoEntry()
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
    , media_unloaded{false}
    , intentional_stop_pending{false}
    , finished_at_eof{false}
    , show_stats{false}
    , hide_ui{false}
    , auto_hide_ui{false}
    , reload_osd_message{}
    , osd{}
{
}