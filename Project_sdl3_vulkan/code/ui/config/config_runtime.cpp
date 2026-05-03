#include "config_runtime.hpp"

#include "video_hover_preview.hpp"
#include "video_seek_preview.hpp"

#include <algorithm>

namespace {

void DrawPreviewSizeControl(const char *title,
                            const char *slider_id,
                            const char *popup_id,
                            ImVec2 &pending_size,
                            ImVec2 &edit_size)
{
    ImGui::TextUnformatted(title);
    ImGui::SliderFloat2(slider_id,
                        &pending_size.x,
                        80.0f,
                        1920.0f,
                        "%.0f px");
    ImGui::SameLine();
    ImGui::TextDisabled("W, H");

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        ImGui::OpenPopup(popup_id);

    if (ImGui::BeginPopup(popup_id)) {
        if (ImGui::IsWindowAppearing())
            edit_size = pending_size;

        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputFloat("Width", &edit_size.x, 1.0f, 10.0f, "%.0f");
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputFloat("Height", &edit_size.y, 1.0f, 10.0f, "%.0f");

        if (ImGui::Button("Set")) {
            pending_size.x = std::clamp(edit_size.x, 80.0f, 1920.0f);
            pending_size.y = std::clamp(edit_size.y, 80.0f, 1920.0f);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();

        ImGui::EndPopup();
    }

    ImGui::TextDisabled("Double-click slider to type exact Width/Height");
}

} // namespace

ConfigRuntime::ConfigRuntime()
    : IsOpen{false}
    , m_pending_hover_size{VideoHoverPreview::preview_size}
    , m_pending_seek_size{VideoSeekPreview::preview_size}
    , m_hover_edit_size{VideoHoverPreview::preview_size}
    , m_seek_edit_size{VideoSeekPreview::preview_size}
    , m_pending_video_resume_threshold_seconds{
        WindowStateToml{}.video_resume_persist_min_duration_seconds}
    , m_applied_video_resume_threshold_seconds{
        WindowStateToml{}.video_resume_persist_min_duration_seconds}
    , m_on_clear_thumbnail_cache{nullptr}
    , m_on_clear_video_cache{nullptr}
    , m_on_rebuild_video_cache{nullptr}
    , m_on_clear_history_metadata{nullptr}
    , m_on_delete_all_cache_and_state{nullptr}
    , m_on_reopen_app{nullptr}
    , m_on_video_resume_threshold_changed{nullptr}
{
}

void ConfigRuntime::SetClearThumbnailCacheCallback(std::function<void()> cb)
{
    m_on_clear_thumbnail_cache = std::move(cb);
}

void ConfigRuntime::SetClearVideoCacheCallback(std::function<void()> cb)
{
    m_on_clear_video_cache = std::move(cb);
}

void ConfigRuntime::SetRebuildVideoCacheCallback(std::function<void()> cb)
{
    m_on_rebuild_video_cache = std::move(cb);
}

void ConfigRuntime::SetClearHistoryMetadataCallback(std::function<void()> cb)
{
    m_on_clear_history_metadata = std::move(cb);
}

void ConfigRuntime::SetDeleteAllCacheAndStateCallback(std::function<void()> cb)
{
    m_on_delete_all_cache_and_state = std::move(cb);
}

void ConfigRuntime::SetReopenAppCallback(std::function<void()> cb)
{
    m_on_reopen_app = std::move(cb);
}

void ConfigRuntime::SetVideoResumeThresholdChangedCallback(std::function<void(int)> cb)
{
    m_on_video_resume_threshold_changed = std::move(cb);
}

int ConfigRuntime::VideoResumeThresholdSeconds() const
{
    return m_applied_video_resume_threshold_seconds;
}

void ConfigRuntime::ApplyLayout(const WindowStateToml &state)
{
    IsOpen = state.show_runtime_config_window;

    if (state.hover_preview_size) {
        VideoHoverPreview::preview_size = ImVec2{state.hover_preview_size->x, state.hover_preview_size->y};
        m_pending_hover_size = VideoHoverPreview::preview_size;
        m_hover_edit_size = m_pending_hover_size;
    }
    if (state.seek_preview_size) {
        VideoSeekPreview::preview_size = ImVec2{state.seek_preview_size->x, state.seek_preview_size->y};
        m_pending_seek_size = VideoSeekPreview::preview_size;
        m_seek_edit_size = m_pending_seek_size;
    }

    m_pending_video_resume_threshold_seconds =
        std::max(state.video_resume_persist_min_duration_seconds, 0);
    m_applied_video_resume_threshold_seconds =
        m_pending_video_resume_threshold_seconds;
    if (m_on_video_resume_threshold_changed)
        m_on_video_resume_threshold_changed(m_applied_video_resume_threshold_seconds);
}

void ConfigRuntime::ExportLayout(WindowStateToml *state) const
{
    state->show_runtime_config_window = IsOpen;
    state->hover_preview_size = WindowStateToml::Vec2Toml{VideoHoverPreview::preview_size.x, VideoHoverPreview::preview_size.y};
    state->seek_preview_size  = WindowStateToml::Vec2Toml{VideoSeekPreview::preview_size.x,  VideoSeekPreview::preview_size.y};
    state->video_resume_persist_min_duration_seconds =
        m_applied_video_resume_threshold_seconds;
}

void ConfigRuntime::Draw()
{
    if (!IsOpen)
        return;

    ImGui::SetNextWindowSize(ImVec2(400.0f, 380.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Runtime Config", &IsOpen))
    {
        ImGui::End();
        return;
    }

    if (ImGui::CollapsingHeader("Video Preview Size", ImGuiTreeNodeFlags_DefaultOpen))
    {
        DrawPreviewSizeControl("Hover Preview Size",
                               "##hover_preview_size",
                               "##hover_preview_size_input",
                               m_pending_hover_size,
                               m_hover_edit_size);
        DrawPreviewSizeControl("Seek Preview Size",
                               "##seek_preview_size",
                               "##seek_preview_size_input",
                               m_pending_seek_size,
                               m_seek_edit_size);

        ImGui::Spacing();
        if (ImGui::Button("Apply"))
        {
            VideoHoverPreview::preview_size = m_pending_hover_size;
            VideoSeekPreview::preview_size  = m_pending_seek_size;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(takes effect for the next video opened)");
    }

    if (ImGui::CollapsingHeader("Thumbnail Cache", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::Button("Clear Thumbnail Cache"))
        {
            if (m_on_clear_thumbnail_cache)
                m_on_clear_thumbnail_cache();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Deletes cached PNG thumbnails on disk");
    }

    if (ImGui::CollapsingHeader("Video Cache", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::Button("Clear Video Cache"))
        {
            if (m_on_clear_video_cache)
                m_on_clear_video_cache();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Deletes cached MP4 downloads on disk");

        if (ImGui::Button("Rebuild Video Cache"))
        {
            if (m_on_rebuild_video_cache)
                m_on_rebuild_video_cache();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Clears old cache and re-queues downloads from video history links");
    }

    if (ImGui::CollapsingHeader("Playback Persistence", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::SliderInt("Resume threshold (seconds)",
                         &m_pending_video_resume_threshold_seconds,
                         0,
                         600,
                         "%d s");
        if (ImGui::Button("Apply Resume Threshold"))
        {
            m_applied_video_resume_threshold_seconds =
                std::max(m_pending_video_resume_threshold_seconds, 0);
            m_pending_video_resume_threshold_seconds =
                m_applied_video_resume_threshold_seconds;
            if (m_on_video_resume_threshold_changed)
                m_on_video_resume_threshold_changed(m_applied_video_resume_threshold_seconds);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Only videos at or above this duration save resume position");
    }

    if (ImGui::CollapsingHeader("History Metadata", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::Button("Clear History Metadata"))
        {
            if (m_on_clear_history_metadata)
                m_on_clear_history_metadata();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Clears history entries from window_state.toml");

        if (ImGui::Button("Delete All Cache + Erase TOML"))
        {
            if (m_on_delete_all_cache_and_state)
                m_on_delete_all_cache_and_state();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Deletes thumbnail/video cache and removes window_state.toml");
    }

    if (ImGui::CollapsingHeader("Application", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::Button("Reopen App"))
        {
            if (m_on_reopen_app)
                m_on_reopen_app();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Closes and restarts the app immediately");
    }

    ImGui::End();
}
