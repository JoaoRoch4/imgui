/**
 * @file main_menu_bar.cpp
 * @brief Implementation of the application main menu bar and top-level
 * coordinator.
 */

#include "pch.hpp"

#include "Image_viewer_panel.hpp"
#include "app_state_coordinator.hpp"
#include "bulk_image_open_queue.hpp"
#include "config_runtime.hpp"
#include "file_browser_context_menu.hpp"
#include "history_context_menu.hpp"
#include "history_preview.hpp"
#include "main_menu_bar.hpp"
#include "media_history_manager.hpp"
#include "media_load_handler.hpp"
#include "open_image_dialogs.hpp"
#include "opened_files_window.hpp"
#include "recent_history_menu.hpp"
#include "style_editor.hpp"
#include "video_context_menu.hpp"
#include "video_downloader.hpp"
#include "video_playback_mode.hpp"
#include "video_player.hpp"
#include "video_player_placebo.hpp"
#include "window_fullscreen_utils.hpp"
#include "window_state_toml.hpp"

#include "FileBrowser.hpp"
#include "imgui_console.hpp"
#include "vulkan_emoji_atlas.hpp"

// ============================================================================
// Constructor / destructor
// ============================================================================

namespace {
ImGui::FileBrowser &GetMainFileExplorer() {
  static ImGui::FileBrowser browser(ImGuiFileBrowserFlags_Window |
                                    ImGuiFileBrowserFlags_EditPathString |
                                    ImGuiFileBrowserFlags_CreateNewDir);
  static bool configured = false;
  if (!configured) {
    browser.SetTitle("File Explorer");
    browser.SetWindowSize(900, 560);
    browser.SetTypeFilters({".*"});
    configured = true;
  }
  return browser;
}

bool &GetShowFileExplorerFlag() {
  static bool show_file_explorer = false;
  return show_file_explorer;
}

bool IsHoverPreviewMediaPath(const std::filesystem::path &path) {
  if (VideoPlayer::is_video_path(path))
    return true;

  const std::string ext = [&]() {
    std::string s = path.extension().string();
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return s;
  }();

  static const std::unordered_set<std::string> k_image_exts = {
      ".jpg", ".jpeg", ".png", ".bmp", ".tga", ".gif", ".webp"};
  return k_image_exts.count(ext) > 0;
}

bool UseVideoPlayerPlacebo() {
  const char *env = std::getenv("IMGUI_USE_VPP");
  if (!env)
    return false;

  std::string value(env);
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value == "1" || value == "true" || value == "on" || value == "yes";
}
} // namespace

MainMenuBar::MainMenuBar()
    : request_quit{false}, request_reopen{false}, m_style_editor{nullptr},
      m_window{nullptr}, m_vk{nullptr}, m_show_demo_window{nullptr},
      m_show_another_window{nullptr},
      m_viewer{std::make_unique<ImageViewerPanel>()},
      m_open_image_dialogs{std::make_unique<OpenImageDialogs>()},
      m_bulk_image_open{std::make_unique<BulkImageOpenQueue>()},
      m_video_player{std::make_unique<VideoPlayer>()},
      m_video_player_placebo{std::make_unique<VideoPlayerPlacebo>()},
      m_video_downloader{std::make_unique<VideoDownloader>()},
      m_config_runtime{std::make_unique<ConfigRuntime>()},
      m_history_preview{std::make_unique<HistoryPreview>()},
      m_opened_files_window{std::make_unique<OpenedFilesWindow>()},
      m_video_context_menu{std::make_unique<VideoContextMenu>()},
      m_fb_context_menu{std::make_unique<FileBrowserContextMenu>()},
      m_history_mgr{std::make_unique<MediaHistoryManager>()},
      m_load_handler{std::make_unique<MediaLoadHandler>()},
      m_app_state{std::make_unique<AppStateCoordinator>()},
      m_use_video_player_placebo{UseVideoPlayerPlacebo()} {}

// Destructor must be defined in the .cpp where all unique_ptr types are
// complete.
MainMenuBar::~MainMenuBar() = default;

// ============================================================================
// Public lifecycle
// ============================================================================

void MainMenuBar::Setup(StyleEditor *style_editor, SDL_Window *window,
                        vulkan_context *vk, bool *show_demo_window,
                        bool *show_another_window,
                        std::function<void(bool)> on_vsync_changed) {
  m_style_editor = style_editor;
  m_window = window;
  m_vk = vk;
  m_show_demo_window = show_demo_window;
  m_show_another_window = show_another_window;

  curl_global_init(CURL_GLOBAL_DEFAULT);

  m_open_image_dialogs->setup(m_window);
  m_viewer->setup(m_window);
  m_video_player->bind_context(m_vk);
  m_video_player->setup(m_vk);
  m_video_player_placebo->bind_context(m_vk);
  m_video_player_placebo->setup(m_vk);

  if (m_use_video_player_placebo) {
    m_video_player_placebo->set_downloader(m_video_downloader.get());
    m_video_player_placebo->set_resume_persist_min_duration_seconds(
        m_config_runtime->VideoResumeThresholdSeconds());
  } else {
    m_video_player->set_downloader(m_video_downloader.get());
    m_video_player->set_resume_persist_min_duration_seconds(
        m_config_runtime->VideoResumeThresholdSeconds());
  }

  m_config_runtime->SetVideoResumeThresholdChangedCallback([this](int seconds) {
    if (m_use_video_player_placebo)
      m_video_player_placebo->set_resume_persist_min_duration_seconds(seconds);
    else
      m_video_player->set_resume_persist_min_duration_seconds(seconds);
  });
  m_config_runtime->SetVsyncChangedCallback(std::move(on_vsync_changed));
  m_video_context_menu->setup(m_window);
  m_video_context_menu->set_on_save_success(
      [this](const std::string &source,
             const std::filesystem::path &saved_path) {
        m_history_mgr->replace_with_saved_file(source, saved_path,
                                               *m_opened_files_window);
        if (m_use_video_player_placebo)
          m_video_player_placebo->replace_source_with_saved_file(source,
                                                                 saved_path);
        else
          m_video_player->replace_source_with_saved_file(source, saved_path);
      });
  m_video_context_menu->set_playback_mode_callbacks(
      [this](const std::string &source) {
        return m_use_video_player_placebo
                   ? m_video_player_placebo->can_toggle_hwdec(source)
                   : m_video_player->can_toggle_hwdec(source);
      },
      [this](const std::string &source) {
        const bool hwdec =
            m_use_video_player_placebo
                ? m_video_player_placebo->is_hwdec_enabled(source)
                : m_video_player->is_hwdec_enabled(source);
        if (m_use_video_player_placebo)
          return static_cast<int>(VideoPlaybackMode::NvdecLibplacebo);
        return hwdec ? static_cast<int>(VideoPlaybackMode::NvdecMpv)
                     : static_cast<int>(VideoPlaybackMode::SwMpv);
      },
      [this](const std::string &source, int mode) {
        const int next_mode = sanitize_video_playback_mode(mode);
        const bool next_hwdec = mode_uses_hwdec(next_mode);
        const bool next_placebo = mode_uses_libplacebo(next_mode);

        const int resume_pos =
            m_use_video_player_placebo
                ? m_video_player_placebo->persisted_position_seconds(source)
                : m_video_player->persisted_position_seconds(source);
        m_history_mgr->set_playback_mode(source, next_mode,
                                         *m_opened_files_window, resume_pos);

        m_use_video_player_placebo = next_placebo;
        m_load_handler->set_use_video_player_placebo(
            m_use_video_player_placebo);
        m_history_preview->set_use_video_player_placebo(
            m_use_video_player_placebo);

        m_video_player->set_all_hwdec(next_hwdec);
        m_video_player_placebo->set_all_hwdec(next_hwdec);
      });
  m_video_context_menu->set_vsync_callbacks(
      [this]() { return m_config_runtime->VsyncEnabled(); },
      [this](bool enabled) { m_config_runtime->SetVsyncEnabled(enabled); });
  m_history_preview->setup(m_vk, m_video_player.get(), m_viewer.get(),
                           m_video_player_placebo.get(),
                           m_use_video_player_placebo);

  // Wire the file explorer hover callback to the preview system.
  GetMainFileExplorer().SetHoverFileCallback(
      [this](const std::filesystem::path &path) {
        if (!IsHoverPreviewMediaPath(path))
          return;

        WindowStateToml::ImageHistoryEntry tmp;
        tmp.source = path.string();
        tmp.title = path.filename().string();
        tmp.kind = "file";
        m_history_preview->draw_for_hover(tmp);
      });

  m_fb_context_menu->setup(m_window);
  GetMainFileExplorer().SetContextMenuCallback(
      [this](const std::filesystem::path &path) {
        auto res = m_fb_context_menu->draw(path);
        if (res.open)
          m_open_image_dialogs->queue_path(res.open_path.string());
      });

  m_app_state->setup(m_history_mgr.get(), m_config_runtime.get(),
                     m_history_preview.get(), m_opened_files_window.get(),
                     m_video_downloader.get());

  const auto bind_player_menus = [this](auto *player) {
    player->set_context_menu(
        m_video_context_menu.get(),
        [this](const std::string &src) -> WindowStateToml::ImageHistoryEntry * {
          for (auto &h : m_history_mgr->entries())
            if (h.source == src)
              return &h;
          return nullptr;
        },
        [this](const std::string &source) {
          m_history_mgr->erase(source, *m_opened_files_window);
        });

    player->set_player_menu_callbacks(
        [this]() { m_open_image_dialogs->begin_open_image_dialog(); },
        [this]() { m_open_image_dialogs->open_url_popup(); },
        [this](const std::string &source, const std::string &kind) {
          if (kind == "file")
            m_open_image_dialogs->queue_path(source);
          else
            m_open_image_dialogs->queue_url(source);
        },
        [this]() -> const std::vector<WindowStateToml::ImageHistoryEntry> & {
          return m_history_mgr->entries();
        },
        m_history_preview.get(),
        [this](const std::string &video_source) {
          m_app_state->toggle_startup_video(video_source);
        },
        [this](const std::string &video_source) {
          return m_app_state->is_startup_video_fixed(video_source);
        },
        [this]() { return is_window_fullscreen(m_window); },
        [this](bool enabled) { set_window_fullscreen(m_window, enabled); });
  };

  if (m_use_video_player_placebo)
    bind_player_menus(m_video_player_placebo.get());
  else
    bind_player_menus(m_video_player.get());

  m_config_runtime->SetClearHistoryMetadataCallback(
      [this]() { m_history_mgr->clear(*m_opened_files_window); });
  m_config_runtime->SetDeleteAllCacheAndStateCallback(
      [this]() { m_app_state->clear_all_cache_and_state(); });
  m_config_runtime->SetReopenAppCallback([this]() {
    request_reopen = true;
    request_quit = true;
  });
  m_config_runtime->SetVideoPlaybackChangedCallback([this](int mode,
                                                           bool loop) {
    const int next_mode = sanitize_video_playback_mode(mode);
    const bool next_hwdec = mode_uses_hwdec(next_mode);
    const bool next_placebo = mode_uses_libplacebo(next_mode);

    m_use_video_player_placebo = next_placebo;
    m_load_handler->set_use_video_player_placebo(m_use_video_player_placebo);
    m_history_preview->set_use_video_player_placebo(m_use_video_player_placebo);

    if (m_use_video_player_placebo) {
      m_video_player_placebo->set_all_hwdec(next_hwdec);
      m_video_player_placebo->set_all_loop(loop);
    } else {
      m_video_player->set_all_hwdec(next_hwdec);
      m_video_player->set_all_loop(loop);
    }
    m_video_player_placebo->reconfigure(VideoPlayerPlacebo::Config{
        .enable_hwdec = next_hwdec,
        .prefer_nvdec = next_hwdec,
    });
  });
  m_config_runtime->SetRestartAllThreadsCallback([this]() {
    if (m_use_video_player_placebo)
      m_video_player_placebo->restart_all_threads();
    else
      m_video_player->restart_all_threads();
  });

  m_opened_files_window->SetEraseHistoryEntryCallback(
      [this](const std::string &source) {
        m_history_mgr->erase(source, *m_opened_files_window);
      });
  m_opened_files_window->SetRestartPreviewCallback([this]() {
    if (m_use_video_player_placebo)
      m_video_player_placebo->restart_hover_preview();
    else
      m_video_player->restart_hover_preview();
  });
  m_opened_files_window->SetRescanTomlCallback([this]() {
    if (!m_app_state->state_path().empty())
      LoadOpenedFilesHistoryFromToml(m_app_state->state_path());
  });
  m_opened_files_window->SetMenuShortcutsCallbacks(
      [this]() { m_open_image_dialogs->begin_open_image_dialog(); },
      [this]() { m_open_image_dialogs->open_url_popup(); },
      [this]() {
        const std::vector<std::string> open_sources =
            m_use_video_player_placebo ? m_video_player_placebo->open_sources()
                                       : m_video_player->open_sources();
        if (open_sources.empty())
          return;
        const bool should_restore =
            !m_app_state->are_all_startup_videos_fixed(open_sources);
        m_app_state->set_startup_video_for_sources(open_sources,
                                                   should_restore);
      },
      [this]() {
        const std::vector<std::string> open_sources =
            m_use_video_player_placebo ? m_video_player_placebo->open_sources()
                                       : m_video_player->open_sources();
        return m_app_state->are_all_startup_videos_fixed(open_sources);
      });

  m_opened_files_window->SetQuitCallback([this]() { request_quit = true; });

  m_load_handler->setup(
      m_viewer.get(), m_video_player.get(), m_video_player_placebo.get(),
      m_bulk_image_open.get(), m_history_mgr.get(), m_video_downloader.get(),
      m_opened_files_window.get(), m_vk, m_use_video_player_placebo);

  // ---- Console -------------------------------------------------------
  m_console = std::make_unique<ConsoleCommands>();
  m_console->OnQuit = [this]() { request_quit = true; };
  m_console->OnDemoToggle = [this](bool on) {
    if (m_show_demo_window)
      *m_show_demo_window = on;
  };
  m_console->OnStyleChange = [](int which) {
    switch (which) {
    case 0:
      ImGui::StyleColorsDark();
      break;
    case 1:
      ImGui::StyleColorsLight();
      break;
    case 2:
      ImGui::StyleColorsClassic();
      break;
    default:
      break;
    }
  };
  m_emoji_atlas = std::make_unique<VulkanEmojiAtlas>(*m_vk);
  // Build the atlas lazily on first Draw() — deferred to avoid blocking Setup.
}

void MainMenuBar::Shutdown() {
  if (!m_vk)
    return;

  m_bulk_image_open->shutdown();
  m_video_player_placebo->shutdown();
  m_video_player->shutdown();
  m_video_downloader->shutdown();
  m_history_preview->shutdown();
  m_viewer->shutdown(*m_vk);
  m_emoji_atlas.reset(); // frees GPU resources before ImGui Vulkan shutdown

  curl_global_cleanup();
}

// ============================================================================
// History + config forwarding
// ============================================================================

void MainMenuBar::ApplyHistory(const WindowStateToml &state) {
  m_app_state->apply_history(state);
}

void MainMenuBar::ApplyRuntimeConfig(const WindowStateToml &state) {
  m_app_state->apply_runtime_config(state);

  auto &browser = GetMainFileExplorer();
  if (!state.file_explorer_recent_directories.empty()) {
    std::vector<std::filesystem::path> dirs;
    dirs.reserve(state.file_explorer_recent_directories.size());
    for (const auto &dir : state.file_explorer_recent_directories) {
      if (!dir.empty())
        dirs.emplace_back(dir);
    }
    browser.SetRecentDirectories(dirs);
  }
  browser.SetSortModeIndex(state.file_explorer_sort_mode);
  if (!state.file_explorer_last_directory.empty())
    browser.SetDirectory(state.file_explorer_last_directory);
  if (state.show_file_explorer_window) {
    GetShowFileExplorerFlag() = true;
    browser.Open();
  }
  m_show_console = state.show_console_window;
}

bool MainMenuBar::LoadOpenedFilesHistoryFromToml(
    const std::filesystem::path &file_path) {
  return m_app_state->load_opened_files_history_from_toml(file_path);
}

void MainMenuBar::SetStatePath(const std::filesystem::path &file_path) {
  m_app_state->set_state_path(file_path);
}

void MainMenuBar::ExportHistory(WindowStateToml *state) {
  if (m_use_video_player_placebo)
    m_video_player_placebo->sync_history_state(m_history_mgr->entries());
  else
    m_video_player->sync_history_state(m_history_mgr->entries());
  m_app_state->export_history(state);
}

void MainMenuBar::ExportRuntimeConfig(WindowStateToml *state) const {
  m_app_state->export_runtime_config(state);

  auto &browser = GetMainFileExplorer();
  state->file_explorer_last_directory = browser.GetDirectory().string();
  state->file_explorer_sort_mode = browser.GetSortModeIndex();
  state->file_explorer_recent_directories.clear();
  for (const auto &dir : browser.GetRecentDirectories())
    state->file_explorer_recent_directories.push_back(dir.string());
  state->show_file_explorer_window = GetShowFileExplorerFlag();
  state->show_console_window = m_show_console;
}

void MainMenuBar::SetThumbDir(const std::filesystem::path &dir) {
  m_app_state->set_thumb_dir(dir);
}

void MainMenuBar::SetDownloadCacheDir(const std::filesystem::path &dir) {
  m_app_state->set_download_cache_dir(dir);
}

// ============================================================================
// Per-frame build
// ============================================================================

void MainMenuBar::Build() {
  // Step 1 — evict closed image windows
  if (m_vk)
    m_viewer->evict_closed(*m_vk);

  // Step 2 — finalise deferred video save
  m_video_context_menu->process_pending_save();

  // Step 2b — file browser context menu deferred ops (delete confirmation
  // modal)
  m_fb_context_menu->process_pending();

  // Step 3 — one-shot startup video restoration
  if (m_app_state->take_restore_videos_on_startup_pending())
    m_load_handler->restore_from_history();

  // Step 4 — drain completed background downloads
  for (const auto &result : m_video_downloader->take_completed()) {
    if (!result.ok)
      continue;

    for (auto &h : m_history_mgr->entries()) {
      if (h.source == result.url) {
        h.cached_path = result.cached_path.string();
        break;
      }
    }
    m_history_mgr->persist();
    if (m_use_video_player_placebo)
      m_video_player_placebo->notify_download_complete(result.url,
                                                       result.cached_path);
    else
      m_video_player->notify_download_complete(result.url, result.cached_path);
  }

  // Steps 5–7 — delegate media loading
  m_load_handler->process_pending_paths(
      m_open_image_dialogs->take_pending_paths());
  m_load_handler->process_pending_urls(
      m_open_image_dialogs->take_pending_urls());
  m_load_handler->drain_bulk_queue();

  // Step 8 — menu bar rendering
  if (!ImGui::BeginMainMenuBar())
    return;

  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Open Image...", "Ctrl+O"))
      m_open_image_dialogs->begin_open_image_dialog();

    if (ImGui::MenuItem("Open Online..."))
      m_open_image_dialogs->open_url_popup();

    auto &history = m_history_mgr->entries();
    if (!history.empty() && ImGui::BeginMenu("Recent")) {
      const auto result = RecentHistoryMenu::draw_entries(
          history,
          {
              .on_open =
                  [this](WindowStateToml::ImageHistoryEntry &entry) {
                    if (entry.kind == "file")
                      m_open_image_dialogs->queue_path(entry.source);
                    else
                      m_open_image_dialogs->queue_url(entry.source);
                  },
              .on_hover =
                  [this](WindowStateToml::ImageHistoryEntry &entry) {
                    m_history_preview->draw_for_hover(entry);
                  },
              .on_after_item =
                  [this](WindowStateToml::ImageHistoryEntry &entry) {
                    const bool is_video =
                        (m_use_video_player_placebo
                             ? VideoPlayerPlacebo::is_video_path(entry.source)
                             : VideoPlayer::is_video_path(entry.source)) ||
                        (m_use_video_player_placebo
                             ? VideoPlayerPlacebo::is_video_url(entry.source)
                             : VideoPlayer::is_video_url(entry.source));

                    if (is_video) {
                      if (const auto r =
                              m_video_context_menu->draw_for_item(entry);
                          r.erase || r.restart_preview || r.quit) {
                        if (r.quit)
                          request_quit = true;
                        if (r.restart_preview) {
                          if (m_use_video_player_placebo)
                            m_video_player_placebo->restart_hover_preview();
                          else
                            m_video_player->restart_hover_preview();
                        }
                        if (r.erase) {
                          m_history_mgr->erase(r.erase_source,
                                               *m_opened_files_window);
                          ImGui::EndMenu();
                          ImGui::EndMenu();
                          return true;
                        }
                      }
                      return false;
                    }

                    if (const auto erase =
                            HistoryContextMenu::draw_for_item(entry.source)) {
                      m_history_mgr->erase(*erase, *m_opened_files_window);
                      ImGui::EndMenu();
                      ImGui::EndMenu();
                      return true;
                    }
                    return false;
                  },
          });

      if (result.shown < static_cast<int>(history.size())) {
        ImGui::Separator();
        ImGui::TextDisabled("(%zu more not shown)",
                            history.size() - static_cast<size_t>(result.shown));
      }

      if (!result.stopped) {
        ImGui::Separator();
        if (ImGui::MenuItem("Clear History"))
          m_history_mgr->clear(*m_opened_files_window);
        ImGui::EndMenu();
      }
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Quit", "Alt+F4"))
      request_quit = true;

    ImGui::EndMenu();
  }

  if (ImGui::BeginMenu("View")) {
    auto &show_file_explorer = GetShowFileExplorerFlag();
    auto &file_explorer = GetMainFileExplorer();

    if (ImGui::MenuItem("File Explorer", nullptr, show_file_explorer)) {
      show_file_explorer = !show_file_explorer;
      if (show_file_explorer)
        file_explorer.Open();
      else
        file_explorer.Close();
    }

    if (ImGui::MenuItem("Console", nullptr, m_show_console))
      m_show_console = !m_show_console;

    if (m_style_editor)
      ImGui::MenuItem("Style Editor", nullptr, &m_style_editor->IsOpen);

    if (m_show_demo_window)
      ImGui::MenuItem("Demo Window", nullptr, m_show_demo_window);

    if (m_show_another_window)
      ImGui::MenuItem("Another Window", nullptr, m_show_another_window);

    ImGui::MenuItem("Opened Files", nullptr, &m_opened_files_window->IsOpen);
    ImGui::MenuItem("Runtime Config", nullptr, &m_config_runtime->IsOpen);

    if (m_viewer->count() > 0) {
      ImGui::Separator();
      m_viewer->build_view_menu_items();
    }

    ImGui::EndMenu();
  }

  ImGui::EndMainMenuBar();

  auto &show_file_explorer = GetShowFileExplorerFlag();
  auto &file_explorer = GetMainFileExplorer();
  if (show_file_explorer) {
    file_explorer.Display();
    if (file_explorer.HasSelected()) {
      const auto selected = file_explorer.GetSelected();
      m_open_image_dialogs->queue_path(selected.string());
      file_explorer.ClearSelected();
    }
    if (!file_explorer.IsOpened())
      show_file_explorer = false;
  }

  // Console window
  if (m_show_console && m_console)
    m_console->Draw("Console##main", &m_show_console);

  // Step 9 — URL popup
  m_open_image_dialogs->draw_url_popup();

  // Steps 10–12 — subsystem rendering
  if (m_use_video_player_placebo)
    m_video_player_placebo->update_frames();
  else
    m_video_player->update_frames();
  m_config_runtime->Draw();
  m_viewer->draw_windows();
  if (m_use_video_player_placebo)
    m_video_player_placebo->draw();
  else
    m_video_player->draw();

  int focus_id = -1;
  const auto activated = m_opened_files_window->draw(
      *m_viewer, *m_history_preview, &focus_id, m_video_context_menu.get());

  if (focus_id >= 0)
    m_viewer->request_focus(focus_id);

  if (activated.has_value()) {
    if (activated->kind == "file")
      m_open_image_dialogs->queue_path(activated->source);
    else if (activated->kind == "url")
      m_open_image_dialogs->queue_url(activated->source);
  }
}