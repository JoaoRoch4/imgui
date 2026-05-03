#pragma once

#include "video_osd_overlay.hpp"
#include "video_seek_preview.hpp"
#include "window_state_toml.hpp"

#include "imgui.h"

#include <SDL3/SDL_stdinc.h>
#include <mpv/client.h>
#include <vulkan/vulkan.h>

#include <functional>
#include <string>
#include <vector>

class HistoryPreview;
class VideoContextMenu;

class VideoUiWindow {
public:
    struct State {
        mpv_handle       *mpv;
        const std::string &display_title;
        std::string      &title;
        std::string      &source;
        std::string      &playback_source;
        std::string      &kind;
        int               id;
        bool             &open;
        bool             &fullscreen;
        bool             &loop;
        bool             &reload_requested;
        bool              load_failed;
        int               video_w;
        int               video_h;
        VkDescriptorSet   descriptor_set;
        VideoOsdOverlay  &osd;
        VideoSeekPreview &seek_preview;
        bool              has_prev;
        bool              has_next;
        bool             &show_stats;
    };

    struct Callbacks {
        VideoContextMenu *context_menu;
        std::function<WindowStateToml::ImageHistoryEntry *(const std::string &)> lookup_history;
        std::function<void(const std::string &)> on_erase_history;
        std::function<void()> on_restart_hover_preview;
        std::function<void()> on_close_all_videos;
        std::function<void()> on_open_image;
        std::function<void()> on_open_online;
        std::function<void(const std::string &, const std::string &)> on_open_recent;
        std::function<const std::vector<WindowStateToml::ImageHistoryEntry> &()> history_provider;
        HistoryPreview *history_preview;
        std::function<void(const std::string &)> on_toggle_startup_video;
        std::function<bool(const std::string &)> is_startup_video_fixed;
        std::function<void(int)> on_switch_relative;
    };

    Uint64 seek_seconds_button_foward = 5;
    int32_t seek_seconds_button_backward = -5;

    void draw(State state, const Callbacks &callbacks) const;
};