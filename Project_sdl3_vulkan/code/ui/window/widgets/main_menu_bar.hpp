#pragma once

#include "Image_viewer_panel.hpp"
#include "app_state_coordinator.hpp"
#include "bulk_image_open_queue.hpp"
#include "config_runtime.hpp"
#include "history_preview.hpp"
#include "media_history_manager.hpp"
#include "media_load_handler.hpp"
#include "open_image_dialogs.hpp"
#include "opened_files_window.hpp"
#include "video_context_menu.hpp"
#include "video_downloader.hpp"
#include "video_player.hpp"
#include "window_state_toml.hpp"


struct SDL_Window;
class StyleEditor;

/**
 * Owns the application main menu bar and coordinates all top-level subsystems.
 *
 * After the refactor, MainMenuBar is responsible only for:
 *   - Owning all subsystems and wiring them together in Setup().
 *   - Rendering the ImGui menu bar and dispatching user actions in Build().
 *   - Forwarding history and config between the TOML persistence layer and
 *     the subsystems that need them.
 *
 * History management (push, erase, clear, persist) is fully delegated to
 * MediaHistoryManager.  Routing of pending paths and URLs to the correct
 * consumer (image viewer, video player, bulk queue) is fully delegated to
 * MediaLoadHandler.
 */
class MainMenuBar {
public:
    MainMenuBar();
    ~MainMenuBar() = default;

    MainMenuBar(const MainMenuBar &) = delete;
    MainMenuBar &operator=(const MainMenuBar &) = delete;

    void Setup(StyleEditor    *style_editor,
               SDL_Window     *window,
               vulkan_context *vk,
               bool           *show_demo_window,
               bool           *show_another_window);

    /// Call once per frame between NewFrame() and Render().
    void Build();

    /// Restore image history and synchronise companion UI from persisted state.
    void ApplyHistory(const WindowStateToml &state);

    /// Restore runtime config values from persisted state.
    void ApplyRuntimeConfig(const WindowStateToml &state);

    /// Load opened-files history directly from a TOML file at startup.
    bool LoadOpenedFilesHistoryFromToml(const std::filesystem::path &file_path);

    /// Set the TOML file path forwarded to MediaHistoryManager for persistence.
    void SetStatePath(const std::filesystem::path &file_path);

    /// Serialise image history into state for the main save routine.
    void ExportHistory(WindowStateToml *state);

    /// Serialise runtime config into state for the main save routine.
    void ExportRuntimeConfig(WindowStateToml *state) const;

    /// Set the directory where hover-thumbnail PNGs are written and cached.
    void SetThumbDir(const std::filesystem::path &dir);

    /// Set the directory where background-downloaded video files are stored.
    void SetDownloadCacheDir(const std::filesystem::path &dir);

    /// Unload all GPU resources. Must be called before ImGui_ImplVulkan_Shutdown.
    void Shutdown();

    bool request_quit; ///< Set to true when File > Quit is selected.
    bool request_reopen; ///< Set to true when Runtime Config requests app reopen.

private:
    // ---- Non-owning external dependencies (provided by App) -----------------
    StyleEditor    *m_style_editor;        ///< Optional style-editor window.
    SDL_Window     *m_window;              ///< Parent SDL window for file dialogs.
    vulkan_context *m_vk;                  ///< Active Vulkan context.
    bool           *m_show_demo_window;    ///< Controls ImGui demo window visibility.
    bool           *m_show_another_window; ///< Controls "Another Window" visibility.

    // ---- Owned subsystems ---------------------------------------------------
    ImageViewerPanel   m_viewer;              ///< Owns all open image windows.
    OpenImageDialogs   m_open_image_dialogs;  ///< File dialog + URL popup state.
    BulkImageOpenQueue m_bulk_image_open;     ///< Off-thread bulk path validator.
    VideoPlayer        m_video_player;        ///< Owns all open video windows.
    VideoDownloader    m_video_downloader;    ///< Background video file caching.
    ConfigRuntime      m_config_runtime;      ///< Runtime settings panel.
    HistoryPreview     m_history_preview;     ///< Hover-thumbnail preview rendering.
    OpenedFilesWindow  m_opened_files_window; ///< "Opened Files" companion list panel.
    VideoContextMenu   m_video_context_menu;  ///< Right-click menu for video entries.

    // ---- Extracted responsibility classes -----------------------------------
    MediaHistoryManager m_history_mgr;  ///< History ownership, push, erase, persist.
    MediaLoadHandler    m_load_handler; ///< Routes pending paths/URLs to consumers.

    AppStateCoordinator m_app_state; ///< History/config/cache forwarding and state persistence wiring.
};