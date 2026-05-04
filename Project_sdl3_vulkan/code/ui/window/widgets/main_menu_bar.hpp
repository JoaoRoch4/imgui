#pragma once
#include "pch.hpp"

struct SDL_Window;
class StyleEditor;
struct WindowStateToml;
class vulkan_context;
class ImageViewerPanel;
class OpenImageDialogs;
class BulkImageOpenQueue;
class VideoPlayer;
class VideoPlayerPlacebo;
class VideoDownloader;
class ConfigRuntime;
class HistoryPreview;
class OpenedFilesWindow;
class VideoContextMenu;
class FileBrowserContextMenu;
class MediaHistoryManager;
class MediaLoadHandler;
class AppStateCoordinator;

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
  ~MainMenuBar();

  MainMenuBar(const MainMenuBar &) = delete;
  MainMenuBar &operator=(const MainMenuBar &) = delete;

  void Setup(StyleEditor *style_editor, SDL_Window *window, vulkan_context *vk,
             bool *show_demo_window, bool *show_another_window,
             std::function<void(bool)> on_vsync_changed = nullptr);

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

  bool request_quit;   ///< Set to true when File > Quit is selected.
  bool request_reopen; ///< Set to true when Runtime Config requests app reopen.

private:
  // ---- Non-owning external dependencies (provided by App) -----------------
  StyleEditor *m_style_editor;
  SDL_Window *m_window;
  vulkan_context *m_vk;
  bool *m_show_demo_window;
  bool *m_show_another_window;

  // ---- Owned subsystems (heap-allocated, incomplete types stay opaque) ----
  std::unique_ptr<ImageViewerPanel> m_viewer;
  std::unique_ptr<OpenImageDialogs> m_open_image_dialogs;
  std::unique_ptr<BulkImageOpenQueue> m_bulk_image_open;
  std::unique_ptr<VideoPlayer> m_video_player;
  std::unique_ptr<VideoPlayerPlacebo> m_video_player_placebo;
  std::unique_ptr<VideoDownloader> m_video_downloader;
  std::unique_ptr<ConfigRuntime> m_config_runtime;
  std::unique_ptr<HistoryPreview> m_history_preview;
  std::unique_ptr<OpenedFilesWindow> m_opened_files_window;
  std::unique_ptr<VideoContextMenu> m_video_context_menu;
  std::unique_ptr<FileBrowserContextMenu> m_fb_context_menu;

  // ---- Extracted responsibility classes -----------------------------------
  std::unique_ptr<MediaHistoryManager> m_history_mgr;
  std::unique_ptr<MediaLoadHandler> m_load_handler;
  std::unique_ptr<AppStateCoordinator> m_app_state;

  bool m_use_video_player_placebo = false;
};