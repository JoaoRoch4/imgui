/**
 * @file main_menu_bar.cpp
 * @brief Implementation of the application main menu bar and top-level coordinator.
 */

#include "main_menu_bar.hpp"
#include "style_editor.hpp"
#include "video_player.hpp"
#include "window_state_toml.hpp"
#include "Image_viewer_panel.hpp"
#include "app_state_coordinator.hpp"
#include "history_preview.hpp"
#include "media_load_handler.hpp"
#include "recent_history_menu.hpp"
#include "video_context_menu.hpp"
#include "opened_files_window.hpp"
#include "config_runtime.hpp"
#include "bulk_image_open_queue.hpp"
#include "open_image_dialogs.hpp"
#include "media_history_manager.hpp"
#include "video_downloader.hpp"
#include "history_context_menu.hpp"

// ============================================================================
// Constructor / destructor
// ============================================================================

MainMenuBar::MainMenuBar()
    : request_quit{false}
    , request_reopen{false}
    , m_style_editor{nullptr}
    , m_window{nullptr}
    , m_vk{nullptr}
    , m_show_demo_window{nullptr}
    , m_show_another_window{nullptr}
    , m_viewer              {std::make_unique<ImageViewerPanel>()}
    , m_open_image_dialogs  {std::make_unique<OpenImageDialogs>()}
    , m_bulk_image_open     {std::make_unique<BulkImageOpenQueue>()}
    , m_video_player        {std::make_unique<VideoPlayer>()}
    , m_video_downloader    {std::make_unique<VideoDownloader>()}
    , m_config_runtime      {std::make_unique<ConfigRuntime>()}
    , m_history_preview     {std::make_unique<HistoryPreview>()}
    , m_opened_files_window {std::make_unique<OpenedFilesWindow>()}
    , m_video_context_menu  {std::make_unique<VideoContextMenu>()}
    , m_history_mgr         {std::make_unique<MediaHistoryManager>()}
    , m_load_handler        {std::make_unique<MediaLoadHandler>()}
    , m_app_state           {std::make_unique<AppStateCoordinator>()}
{}

// Destructor must be defined in the .cpp where all unique_ptr types are complete.
MainMenuBar::~MainMenuBar() = default;

// ============================================================================
// Public lifecycle
// ============================================================================

void MainMenuBar::Setup(StyleEditor    *style_editor,
                        SDL_Window     *window,
                        vulkan_context *vk,
                        bool           *show_demo_window,
                        bool           *show_another_window)
{
    m_style_editor        = style_editor;
    m_window              = window;
    m_vk                  = vk;
    m_show_demo_window    = show_demo_window;
    m_show_another_window = show_another_window;

    curl_global_init(CURL_GLOBAL_DEFAULT);

    m_open_image_dialogs->setup(m_window);
    m_video_player->setup(m_vk);
    m_video_player->set_downloader(m_video_downloader.get());
    m_video_player->set_resume_persist_min_duration_seconds(
        m_config_runtime->VideoResumeThresholdSeconds());
    m_config_runtime->SetVideoResumeThresholdChangedCallback(
        [this](int seconds) {
            m_video_player->set_resume_persist_min_duration_seconds(seconds);
        });
    m_video_context_menu->setup(m_window);
    m_video_context_menu->set_on_save_success(
        [this](const std::string &source, const std::filesystem::path &saved_path) {
            m_history_mgr->replace_with_saved_file(source, saved_path, *m_opened_files_window);
            m_video_player->replace_source_with_saved_file(source, saved_path);
        });
    m_video_context_menu->set_hwdec_callbacks(
        [this](const std::string &source) {
            return m_video_player->can_toggle_hwdec(source);
        },
        [this](const std::string &source) {
            return m_video_player->is_hwdec_enabled(source);
        },
        [this](const std::string &source) {
            const bool next_enabled = !m_video_player->is_hwdec_enabled(source);
            m_history_mgr->set_hwdec_enabled(source,
                                             next_enabled,
                                             *m_opened_files_window,
                                             m_video_player->persisted_position_seconds(source));
            m_video_player->toggle_hwdec(source);
        });
    m_history_preview->setup(m_vk, m_video_player.get(), m_viewer.get());
    m_app_state->setup(
        m_history_mgr.get(),
        m_config_runtime.get(),
        m_history_preview.get(),
        m_opened_files_window.get(),
        m_video_downloader.get());

    m_video_player->set_context_menu(
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

    m_video_player->set_player_menu_callbacks(
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
        });

    m_config_runtime->SetClearHistoryMetadataCallback([this]() {
        m_history_mgr->clear(*m_opened_files_window);
    });
    m_config_runtime->SetDeleteAllCacheAndStateCallback([this]() {
        m_app_state->clear_all_cache_and_state();
    });
    m_config_runtime->SetReopenAppCallback([this]() {
        request_reopen = true;
        request_quit   = true;
    });

    m_opened_files_window->SetEraseHistoryEntryCallback([this](const std::string &source) {
        m_history_mgr->erase(source, *m_opened_files_window);
    });
    m_opened_files_window->SetRestartPreviewCallback([this]() {
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
            const std::vector<std::string> open_sources = m_video_player->open_sources();
            if (open_sources.empty())
                return;
            const bool should_restore = !m_app_state->are_all_startup_videos_fixed(open_sources);
            m_app_state->set_startup_video_for_sources(open_sources, should_restore);
        },
        [this]() {
            const std::vector<std::string> open_sources = m_video_player->open_sources();
            return m_app_state->are_all_startup_videos_fixed(open_sources);
        });

    m_load_handler->setup(
        m_viewer.get(),
        m_video_player.get(),
        m_bulk_image_open.get(),
        m_history_mgr.get(),
        m_video_downloader.get(),
        m_opened_files_window.get(),
        m_vk);
}

void MainMenuBar::Shutdown()
{
    if (!m_vk)
        return;

    m_bulk_image_open->shutdown();
    m_video_player->shutdown();
    m_video_downloader->shutdown();
    m_history_preview->shutdown();
    m_viewer->shutdown(*m_vk);

    curl_global_cleanup();
}

// ============================================================================
// History + config forwarding
// ============================================================================

void MainMenuBar::ApplyHistory(const WindowStateToml &state)
{
    m_app_state->apply_history(state);
}

void MainMenuBar::ApplyRuntimeConfig(const WindowStateToml &state)
{
    m_app_state->apply_runtime_config(state);
}

bool MainMenuBar::LoadOpenedFilesHistoryFromToml(const std::filesystem::path &file_path)
{
    return m_app_state->load_opened_files_history_from_toml(file_path);
}

void MainMenuBar::SetStatePath(const std::filesystem::path &file_path)
{
    m_app_state->set_state_path(file_path);
}

void MainMenuBar::ExportHistory(WindowStateToml *state)
{
    m_video_player->sync_history_state(m_history_mgr->entries());
    m_app_state->export_history(state);
}

void MainMenuBar::ExportRuntimeConfig(WindowStateToml *state) const
{
    m_app_state->export_runtime_config(state);
}

void MainMenuBar::SetThumbDir(const std::filesystem::path &dir)
{
    m_app_state->set_thumb_dir(dir);
}

void MainMenuBar::SetDownloadCacheDir(const std::filesystem::path &dir)
{
    m_app_state->set_download_cache_dir(dir);
}

// ============================================================================
// Per-frame build
// ============================================================================

void MainMenuBar::Build()
{
    // Step 1 — evict closed image windows
    if (m_vk)
        m_viewer->evict_closed(*m_vk);

    // Step 2 — finalise deferred video save
    m_video_context_menu->process_pending_save();

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
        m_video_player->notify_download_complete(result.url, result.cached_path);
    }

    // Steps 5–7 — delegate media loading
    m_load_handler->process_pending_paths(m_open_image_dialogs->take_pending_paths());
    m_load_handler->process_pending_urls(m_open_image_dialogs->take_pending_urls());
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
                    .on_open = [this](WindowStateToml::ImageHistoryEntry &entry) {
                        if (entry.kind == "file")
                            m_open_image_dialogs->queue_path(entry.source);
                        else
                            m_open_image_dialogs->queue_url(entry.source);
                    },
                    .on_hover = [this](WindowStateToml::ImageHistoryEntry &entry) {
                        m_history_preview->draw_for_hover(entry);
                    },
                    .on_after_item = [this](WindowStateToml::ImageHistoryEntry &entry) {
                        const bool is_video = VideoPlayer::is_video_path(entry.source) ||
                                              VideoPlayer::is_video_url(entry.source);

                        if (is_video) {
                            if (const auto r = m_video_context_menu->draw_for_item(entry);
                                r.erase || r.restart_preview) {
                                if (r.restart_preview)
                                    m_video_player->restart_hover_preview();
                                if (r.erase) {
                                    m_history_mgr->erase(r.erase_source, *m_opened_files_window);
                                    ImGui::EndMenu();
                                    ImGui::EndMenu();
                                    return true;
                                }
                            }
                            return false;
                        }

                        if (const auto erase = HistoryContextMenu::draw_for_item(entry.source)) {
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
        if (m_style_editor)
            ImGui::MenuItem("Style Editor", nullptr, &m_style_editor->IsOpen);

        if (m_show_demo_window)
            ImGui::MenuItem("Demo Window", nullptr, m_show_demo_window);

        if (m_show_another_window)
            ImGui::MenuItem("Another Window", nullptr, m_show_another_window);

        ImGui::MenuItem("Opened Files",   nullptr, &m_opened_files_window->IsOpen);
        ImGui::MenuItem("Runtime Config", nullptr, &m_config_runtime->IsOpen);

        if (m_viewer->count() > 0) {
            ImGui::Separator();
            m_viewer->build_view_menu_items();
        }

        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();

    // Step 9 — URL popup
    m_open_image_dialogs->draw_url_popup();

    // Steps 10–12 — subsystem rendering
    m_video_player->update_frames();
    m_config_runtime->Draw();
    m_viewer->draw_windows();
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