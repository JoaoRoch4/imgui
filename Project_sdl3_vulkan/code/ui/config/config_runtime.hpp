#pragma once

#include "window_state_toml.hpp"

#include <functional>

/// Runtime configuration window.
/// Exposes settings that can be changed while the application is running,
/// such as video preview thumbnail dimensions.
class ConfigRuntime {
public:
    ConfigRuntime();

    bool IsOpen;

    /// Draw the configuration window (no-op when IsOpen == false).
    void Draw();

    /// Restore values from persisted state (call after LoadWindowStateToml).
    void ApplyLayout(const WindowStateToml &state);

    /// Write current values into state (call before SaveWindowStateToml).
    void ExportLayout(WindowStateToml *state) const;

    /// Register a callback invoked when the user clicks "Clear Thumbnail Cache".
    void SetClearThumbnailCacheCallback(std::function<void()> cb);

    /// Register a callback invoked when the user clicks "Clear Video Cache".
    void SetClearVideoCacheCallback(std::function<void()> cb);

    /// Register a callback invoked when the user clicks "Rebuild Video Cache".
    void SetRebuildVideoCacheCallback(std::function<void()> cb);

    /// Register a callback invoked when the user clicks "Clear History Metadata".
    void SetClearHistoryMetadataCallback(std::function<void()> cb);

    /// Register a callback invoked when the user clicks "Delete All Cache + Erase TOML".
    void SetDeleteAllCacheAndStateCallback(std::function<void()> cb);

    /// Register a callback invoked when the user clicks "Reopen App".
    void SetReopenAppCallback(std::function<void()> cb);

    /// Register a callback invoked when the video resume threshold is applied.
    void SetVideoResumeThresholdChangedCallback(std::function<void(int)> cb);

    [[nodiscard]] int VideoResumeThresholdSeconds() const;

private:
    ImVec2 m_pending_hover_size;
    ImVec2 m_pending_seek_size;
    ImVec2 m_hover_edit_size;
    ImVec2 m_seek_edit_size;
    int m_pending_video_resume_threshold_seconds;
    int m_applied_video_resume_threshold_seconds;
    std::function<void()> m_on_clear_thumbnail_cache;
    std::function<void()> m_on_clear_video_cache;
    std::function<void()> m_on_rebuild_video_cache;
    std::function<void()> m_on_clear_history_metadata;
    std::function<void()> m_on_delete_all_cache_and_state;
    std::function<void()> m_on_reopen_app;
    std::function<void(int)> m_on_video_resume_threshold_changed;
};
