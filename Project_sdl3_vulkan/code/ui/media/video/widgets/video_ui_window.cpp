#include "video_ui_window.hpp"

#include "history_preview.hpp"
#include "recent_history_menu.hpp"
#include "video_context_menu.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace {

std::string format_time(double time_seconds)
{
    const int total_seconds = static_cast<int>(time_seconds);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", total_seconds / 60, total_seconds % 60);
    return std::string(buffer);
}

void draw_recent_menu(const VideoUiWindow::Callbacks &callbacks)
{
    if (!callbacks.history_provider || !callbacks.on_open_recent)
        return;

    const auto &history = callbacks.history_provider();
    if (history.empty() || !ImGui::BeginMenu("Recent"))
        return;

    RecentHistoryMenu::draw_entries(
        history,
        {
            .on_open = [&callbacks](WindowStateToml::ImageHistoryEntry &entry) {
                callbacks.on_open_recent(entry.source, entry.kind);
            },
            .on_hover = [&callbacks](WindowStateToml::ImageHistoryEntry &entry) {
                if (!callbacks.history_preview || !callbacks.lookup_history)
                    return;
                if (auto *found = callbacks.lookup_history(entry.source))
                    callbacks.history_preview->draw_for_hover(*found);
            },
            .on_after_item = nullptr,
        });

    ImGui::EndMenu();
}

void draw_open_shortcuts(const std::string &source,
                         const char        *startup_label,
                         const VideoUiWindow::Callbacks &callbacks)
{
    if (callbacks.on_open_image && ImGui::MenuItem("Open Image...", "Ctrl+O"))
        callbacks.on_open_image();
    if (callbacks.on_open_online && ImGui::MenuItem("Open Online..."))
        callbacks.on_open_online();
    if (callbacks.on_toggle_startup_video && ImGui::MenuItem(startup_label))
        callbacks.on_toggle_startup_video(source);

    draw_recent_menu(callbacks);
}

struct StatsCache {
    std::chrono::steady_clock::time_point next_sample;
    std::array<std::string, 5> lines;
};

void draw_stats_overlay(mpv_handle *mpv, ImVec2 image_pos, ImVec2 display_size)
{
    static std::unordered_map<mpv_handle *, StatsCache> s_cache;

    StatsCache &cache = s_cache[mpv];
    const auto now = std::chrono::steady_clock::now();
    if (now >= cache.next_sample) {
        cache.next_sample = now + std::chrono::milliseconds(500);

        int64_t video_bitrate = 0;
        int64_t audio_bitrate = 0;
        int64_t frame_drop    = 0;
        int64_t mistimed      = 0;
        mpv_get_property(mpv, "video-bitrate",        MPV_FORMAT_INT64, &video_bitrate);
        mpv_get_property(mpv, "audio-bitrate",        MPV_FORMAT_INT64, &audio_bitrate);
        mpv_get_property(mpv, "frame-drop-count",     MPV_FORMAT_INT64, &frame_drop);
        mpv_get_property(mpv, "mistimed-frame-count", MPV_FORMAT_INT64, &mistimed);

        int vo_pass_count = 0;
        mpv_node passes_node{};
        if (mpv_get_property(mpv, "vo-passes", MPV_FORMAT_NODE, &passes_node) >= 0) {
            if (passes_node.format == MPV_FORMAT_NODE_MAP) {
                for (int i = 0; i < passes_node.u.list->num; ++i) {
                    if (std::strcmp(passes_node.u.list->keys[i], "fresh") == 0) {
                        const mpv_node &fresh = passes_node.u.list->values[i];
                        if (fresh.format == MPV_FORMAT_NODE_ARRAY)
                            vo_pass_count = fresh.u.list->num;
                        break;
                    }
                }
            }
            mpv_free_node_contents(&passes_node);
        }

        auto format_bps = [](int64_t bps) -> std::string {
            std::array<char, 32> buf{};
            if (bps >= 1'000'000)
                std::snprintf(buf.data(), buf.size(), "%.1f Mbps",
                              static_cast<double>(bps) / 1'000'000.0);
            else
                std::snprintf(buf.data(), buf.size(), "%lld Kbps",
                              static_cast<long long>(bps / 1000));
            return std::string(buf.data());
        };

        cache.lines = {
            "Video:     " + format_bps(video_bitrate),
            "Audio:     " + format_bps(audio_bitrate),
            "VO Passes: " + std::to_string(vo_pass_count),
            "Dropped:   " + std::to_string(frame_drop),
            "Mistimed:  " + std::to_string(mistimed),
        };
    }

    const std::array<std::string, 5> &lines = cache.lines;

    const float pad_x      = 10.0f;
    const float pad_y      = 8.0f;
    const float line_gap   = 3.0f;
    const float line_h     = ImGui::GetTextLineHeight();

    float max_w = 0.0f;
    for (const auto &l : lines)
        max_w = std::max(max_w, ImGui::CalcTextSize(l.c_str()).x);

    const int   n         = static_cast<int>(lines.size());
    const float box_w     = max_w + pad_x * 2.0f;
    const float box_h     = static_cast<float>(n) * (line_h + line_gap) - line_gap + pad_y * 2.0f;
    const ImVec2 box_min  = {image_pos.x + 8.0f, image_pos.y + 8.0f};
    const ImVec2 box_max  = {box_min.x + box_w, box_min.y + box_h};

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(box_min, box_max,
                      ImGui::GetColorU32(ImVec4(0.04f, 0.04f, 0.04f, 0.82f)), 6.0f);
    dl->AddRect(box_min, box_max,
                ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.18f)), 6.0f);
    for (int i = 0; i < n; ++i) {
        dl->AddText(
            ImVec2(box_min.x + pad_x,
                   box_min.y + pad_y + static_cast<float>(i) * (line_h + line_gap)),
            ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f)),
            lines[static_cast<size_t>(i)].c_str());
    }
}

} // namespace

void VideoUiWindow::draw(State state, const Callbacks &callbacks) const
{
    const bool startup_fixed = callbacks.is_startup_video_fixed &&
                               callbacks.is_startup_video_fixed(state.source);
    const char *startup_label = startup_fixed ? "Unfix Startup Videos" : "Set Startup Videos";

    const auto toggle_pause = [&state]() {
        int paused = 0;
        mpv_get_property(state.mpv, "pause", MPV_FORMAT_FLAG, &paused);
        int next_paused = paused ? 0 : 1;
        mpv_set_property(state.mpv, "pause", MPV_FORMAT_FLAG, &next_paused);
        state.osd.show(next_paused ? "Paused" : "Playing");
    };

    const auto seek_by = [&state](int delta_seconds) {
        const std::string command = "seek " + std::to_string(delta_seconds);
        mpv_command_string(state.mpv, command.c_str());
        state.osd.show(delta_seconds >= 0
                           ? "Seek +" + std::to_string(delta_seconds) + "s"
                           : "Seek " + std::to_string(delta_seconds) + "s");
    };

    ImGuiWindowFlags flags = ImGuiWindowFlags_None;
    if (state.fullscreen) {
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->Pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(viewport->Size, ImGuiCond_Always);
        flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus;
    } else {
        ImGui::SetNextWindowSize(ImVec2(640.0f, 420.0f), ImGuiCond_FirstUseEver);
        if (callbacks.on_open_image || callbacks.on_open_online || callbacks.on_open_recent)
            flags |= ImGuiWindowFlags_MenuBar;
    }

    const std::string window_id = state.display_title + "###video_" + std::to_string(state.id);
    if (!ImGui::Begin(window_id.c_str(), &state.open, flags)) {
        ImGui::End();
        return;
    }

    if (callbacks.context_menu) {
        WindowStateToml::ImageHistoryEntry history_entry;
        if (callbacks.lookup_history) {
            if (const auto *found = callbacks.lookup_history(state.source))
                history_entry = *found;
        }
        if (history_entry.source.empty()) {
            history_entry.source = state.source;
            history_entry.kind = state.kind;
        }

        const std::string popup_id = "##vctx_" + std::to_string(state.id);
        if (ImGui::BeginPopupContextWindow(popup_id.c_str())) {
            const auto context_result = callbacks.context_menu->draw_menu_items(history_entry);
            if (context_result.erase && callbacks.on_erase_history)
                callbacks.on_erase_history(context_result.erase_source);
            if (context_result.restart_preview && callbacks.on_restart_hover_preview)
                callbacks.on_restart_hover_preview();

            ImGui::Separator();
            if (ImGui::MenuItem("Reload Video")) {
                state.reload_requested = true;
                state.osd.show("Reloading...");
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Show Status", nullptr, state.show_stats))
                state.show_stats = !state.show_stats;
            if (ImGui::MenuItem("Close Video")) {
                state.open = false;
                ImGui::CloseCurrentPopup();
            }
            if (ImGui::MenuItem("Close All Videos")) {
                if (callbacks.on_close_all_videos)
                    callbacks.on_close_all_videos();
                ImGui::CloseCurrentPopup();
            }

            if (callbacks.on_open_image || callbacks.on_open_online || callbacks.on_open_recent) {
                ImGui::Separator();
                draw_open_shortcuts(state.source, startup_label, callbacks);
            }
            ImGui::EndPopup();
        }
    }

    if (!state.fullscreen && ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            draw_open_shortcuts(state.source, startup_label, callbacks);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    if (state.fullscreen && ImGui::IsKeyPressed(ImGuiKey_Escape))
        state.fullscreen = false;

    constexpr float k_controls_height = 72.0f;
    const ImVec2 available = ImGui::GetContentRegionAvail();

    if (state.descriptor_set != VK_NULL_HANDLE && state.video_w > 0 && state.video_h > 0) {
        const float video_aspect = static_cast<float>(state.video_w) / static_cast<float>(state.video_h);
        const float canvas_height = std::max(available.y - k_controls_height, 1.0f);
        const float canvas_width = available.x;

        float display_width = 0.0f;
        float display_height = 0.0f;
        if (video_aspect > canvas_width / canvas_height) {
            display_width = canvas_width;
            display_height = canvas_width / video_aspect;
        } else {
            display_height = canvas_height;
            display_width = canvas_height * video_aspect;
        }

        display_width = std::max(display_width, 1.0f);
        display_height = std::max(display_height, 1.0f);

        const ImVec2 cursor_base = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(cursor_base.x + (canvas_width - display_width) * 0.5f,
                                   cursor_base.y + (canvas_height - display_height) * 0.5f));
        const ImVec2 image_pos = ImGui::GetCursorScreenPos();
        ImGui::Image(std::bit_cast<ImTextureID>(state.descriptor_set),
                     ImVec2(display_width, display_height));
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                state.fullscreen = !state.fullscreen;
                state.osd.show(state.fullscreen ? "Fullscreen" : "Windowed");
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                toggle_pause();
            }
        }
        state.osd.draw(image_pos, ImVec2(display_width, display_height));
        if (state.show_stats)
            draw_stats_overlay(state.mpv, image_pos, ImVec2(display_width, display_height));
    } else if (state.load_failed) {
        ImGui::TextDisabled("Failed to load");
    } else {
        ImGui::TextDisabled("Loading...");
    }

    ImGui::Separator();

    double time_pos = 0.0;
    double duration = 0.0;
    int paused = 0;
    int64_t volume = 100;
    mpv_get_property(state.mpv, "time-pos", MPV_FORMAT_DOUBLE, &time_pos);
    mpv_get_property(state.mpv, "duration", MPV_FORMAT_DOUBLE, &duration);
    mpv_get_property(state.mpv, "pause", MPV_FORMAT_FLAG, &paused);
    mpv_get_property(state.mpv, "volume", MPV_FORMAT_INT64, &volume);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 1.0f));

    if (!state.has_prev)
        ImGui::BeginDisabled();
    if (ImGui::SmallButton("|<") && callbacks.on_switch_relative)
        callbacks.on_switch_relative(-1);
    if (!state.has_prev)
        ImGui::EndDisabled();

    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::SmallButton("<<"))
        seek_by(-10);

    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::SmallButton(paused ? "|>" : "||"))
        toggle_pause();

    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::SmallButton(">>"))
        seek_by(10);

    ImGui::SameLine(0.0f, 4.0f);
    const bool loop_style_pushed = state.loop;
    if (loop_style_pushed)
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::SmallButton("⟳")) {
        state.loop = !state.loop;
        mpv_command_string(state.mpv, state.loop ? "set loop-file inf" : "set loop-file no");
        state.osd.show(state.loop ? "Loop On" : "Loop Off");
    }
    if (loop_style_pushed)
        ImGui::PopStyleColor();

    ImGui::SameLine(0.0f, 4.0f);
    if (!state.has_next)
        ImGui::BeginDisabled();
    if (ImGui::SmallButton(">|") && callbacks.on_switch_relative)
        callbacks.on_switch_relative(1);
    if (!state.has_next)
        ImGui::EndDisabled();

    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::SmallButton("Reload")) {
        state.reload_requested = true;
        state.osd.show("Reloading...");
    }

    ImGui::PopStyleVar();

    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextUnformatted(format_time(time_pos).c_str());
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::TextDisabled("/");
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::TextUnformatted(format_time(duration).c_str());

    if (duration > 0.0) {
        float pos_fraction = static_cast<float>(time_pos / duration);
        ImGui::SetNextItemWidth(-140.0f);
        if (ImGui::SliderFloat("##seek", &pos_fraction, 0.0f, 1.0f, "")) {
            double new_pos = pos_fraction * duration;
            mpv_set_property(state.mpv, "time-pos", MPV_FORMAT_DOUBLE, &new_pos);
        }

        if (ImGui::IsItemHovered() && state.seek_preview.descriptor_set() != VK_NULL_HANDLE) {
            const ImVec2 item_min = ImGui::GetItemRectMin();
            const ImVec2 item_max = ImGui::GetItemRectMax();
            const float fraction = std::clamp(
                (ImGui::GetMousePos().x - item_min.x) / (item_max.x - item_min.x),
                0.0f,
                1.0f);
            state.seek_preview.seek(static_cast<double>(fraction) * duration);
            ImGui::BeginTooltip();
            ImGui::Image(std::bit_cast<ImTextureID>(state.seek_preview.descriptor_set()),
                         state.seek_preview.size());
            ImGui::Text("%s", format_time(static_cast<double>(fraction) * duration).c_str());
            ImGui::EndTooltip();
        }

        ImGui::SameLine();
    }

    int volume_percent = static_cast<int>(volume);
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::SliderInt("##vol", &volume_percent, 0, 150, "Vol %d%%")) {
        int64_t new_volume = volume_percent;
        mpv_set_property(state.mpv, "volume", MPV_FORMAT_INT64, &new_volume);
        state.osd.show("Volume " + std::to_string(volume_percent) + "%");
    }

    ImGui::End();
}