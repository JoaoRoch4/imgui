/**
 * @file main_menu_bar.cpp
 * @brief Implementation of the application main menu bar and top-level coordinator.
 *
 * After the refactor, this file is responsible only for:
 *   - Wiring all subsystems together in Setup().
 *   - Rendering the ImGui menu bar in Build().
 *   - Forwarding TOML persistence calls to MediaHistoryManager.
 *
 * History operations (push, erase, clear, persist) are fully delegated to
 * MediaHistoryManager.  Routing of pending paths and URLs to the correct
 * consumer is fully delegated to MediaLoadHandler.
 */

#include "main_menu_bar.hpp"
#include "history_context_menu.hpp"
#include "recent_history_menu.hpp"
#include "image_downloader.hpp"
#include "style_editor.hpp"
#include "video_player.hpp"
#include "window_state_toml.hpp"


#include <curl/curl.h>

#include <unordered_set>

// ============================================================================
// Constructor
// ============================================================================

/**
 * @brief Initialise all pointer members to null and flags to their default state.
 */
MainMenuBar::MainMenuBar()
    : request_quit{false}           // no quit requested on startup
    , request_reopen{false}
    , m_style_editor{nullptr}       // provided externally in Setup()
    , m_window{nullptr}             // provided externally in Setup()
    , m_vk{nullptr}                 // provided externally in Setup()
    , m_show_demo_window{nullptr}   // provided externally in Setup()
    , m_show_another_window{nullptr}// provided externally in Setup()
    , m_viewer{}                    // default-constructs ImageViewerPanel
    , m_open_image_dialogs{}        // default-constructs dialog state
    , m_bulk_image_open{}           // default-constructs background queue
    , m_video_player{}              // default-constructs player
    , m_video_downloader{}          // default-constructs downloader
    , m_config_runtime{}            // default-constructs runtime config panel
    , m_history_preview{}           // default-constructs preview renderer
    , m_opened_files_window{}       // default-constructs companion list panel
    , m_video_context_menu{}        // default-constructs right-click menu
    , m_history_mgr{}               // owns the history vector + TOML persistence
    , m_load_handler{}              // routes pending paths/URLs to consumers
    , m_app_state{}                 // extracted history/config/cache coordinator
{
}

// ============================================================================
// Public lifecycle
// ============================================================================

/**
 * @brief Store external dependencies, initialise libcurl, and wire all subsystems.
 *
 * Callbacks capture `this` so all lambdas below must not outlive MainMenuBar.
 *
 * @param style_editor        Pointer to the style-editor window (may be null).
 * @param window              The SDL3 parent window used for native file dialogs.
 * @param vk                  Active Vulkan context.
 * @param show_demo_window    Pointer to the ImGui demo-window visibility flag.
 * @param show_another_window Pointer to the "Another Window" visibility flag.
 */
void MainMenuBar::Setup(StyleEditor    *style_editor,
                        SDL_Window     *window,
                        vulkan_context *vk,
                        bool           *show_demo_window,
                        bool           *show_another_window)
{
    // Store the externally owned dependencies for use across all frames.
    m_style_editor        = style_editor;
    m_window              = window;
    m_vk                  = vk;
    m_show_demo_window    = show_demo_window;
    m_show_another_window = show_another_window;

    // Initialise the libcurl global state once for the entire process lifetime.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Initialise subsystems that require the SDL window or Vulkan context.
    m_open_image_dialogs.setup(m_window);   // needs window for dialog parent
    m_video_player.setup(m_vk);             // needs Vulkan for frame decode
    m_video_player.set_downloader(&m_video_downloader); // download progress in titles
    m_video_player.set_resume_persist_min_duration_seconds(
        m_config_runtime.VideoResumeThresholdSeconds());
    m_config_runtime.SetVideoResumeThresholdChangedCallback(
        [this](int seconds) {
            m_video_player.set_resume_persist_min_duration_seconds(seconds);
        });
    m_video_context_menu.setup(m_window);   // needs window for save dialog
    m_video_context_menu.set_on_save_success(
        [this](const std::string &source, const std::filesystem::path &saved_path) {
            m_history_mgr.replace_with_saved_file(source, saved_path, m_opened_files_window);
            m_video_player.replace_source_with_saved_file(source, saved_path);
        });
    m_video_context_menu.set_hwdec_callbacks(
        [this](const std::string &source) {
            return m_video_player.can_toggle_hwdec(source);
        },
        [this](const std::string &source) {
            return m_video_player.is_hwdec_enabled(source);
        },
        [this](const std::string &source) {
            const bool next_enabled = !m_video_player.is_hwdec_enabled(source);
            m_history_mgr.set_hwdec_enabled(source,
                                           next_enabled,
                                           m_opened_files_window,
                                           m_video_player.persisted_position_seconds(source));
            m_video_player.toggle_hwdec(source);
        });
    m_history_preview.setup(m_vk, &m_video_player, &m_viewer); // thumbnail renderer
    m_app_state.setup(
        &m_history_mgr,
        &m_config_runtime,
        &m_history_preview,
        &m_opened_files_window,
        &m_video_downloader);

    // ---- VideoPlayer context-menu callbacks --------------------------------
    // Provide history lookup and deletion so the video context menu can
    // find and remove its own entry without knowing about the history class.
    m_video_player.set_context_menu(
        &m_video_context_menu,

        // Lookup: return a pointer into the history vector (nullptr if not found).
        [this](const std::string &src) -> WindowStateToml::ImageHistoryEntry * {
            for (auto &h : m_history_mgr.entries())
                if (h.source == src)
                    return &h; // direct pointer into the vector — valid until erase
            return nullptr;
        },

        // Delete: remove the entry from history, sync UI, persist to disk.
        [this](const std::string &source) {
            m_history_mgr.erase(source, m_opened_files_window);
        });

    // ---- VideoPlayer menu callbacks ----------------------------------------
    // Let the embedded video-player menu trigger the file/URL dialogs and
    // read the current history without depending on MainMenuBar directly.
    m_video_player.set_player_menu_callbacks(
        // Open Image... menu item inside the player window.
        [this]() { m_open_image_dialogs.begin_open_image_dialog(); },

        // Open Online... menu item inside the player window.
        [this]() { m_open_image_dialogs.open_online_popup(); },

        // Re-open a history entry selected inside the player window.
        [this](const std::string &source, const std::string &kind) {
            if (kind == "file")
                m_open_image_dialogs.queue_path(source); // re-open local file
            else
                m_open_image_dialogs.queue_url(source);  // re-open URL
        },

        // Supply a const view of the history to the player for its recent list.
        [this]() -> const std::vector<WindowStateToml::ImageHistoryEntry> & {
            return m_history_mgr.entries();
        },

        &m_history_preview, // hover thumbnail renderer used by the player menu

        // Startup-video set/unfix action button inside the player window.
        [this](const std::string &video_source) {
            m_app_state.toggle_startup_video(video_source);
        },
        [this](const std::string &video_source) {
            return m_app_state.is_startup_video_fixed(video_source);
        });

    // ---- ConfigRuntime callbacks -------------------------------------------
    // These lambdas run when the user presses action buttons in the settings panel.

    // Clear all history entries from memory and disk.
    m_config_runtime.SetClearHistoryMetadataCallback([this]() {
        m_history_mgr.clear(m_opened_files_window); // clears, syncs UI, persists
    });

    // Clear thumbnail/video cache and remove the persisted state file.
    m_config_runtime.SetDeleteAllCacheAndStateCallback([this]() {
        m_app_state.clear_all_cache_and_state();
    });

    m_config_runtime.SetReopenAppCallback([this]() {
        request_reopen = true;
        request_quit = true;
    });

    // ---- OpenedFilesWindow callbacks ----------------------------------------
    // Allow the companion list panel to delete individual history entries.
    m_opened_files_window.SetEraseHistoryEntryCallback([this](const std::string &source) {
        m_history_mgr.erase(source, m_opened_files_window); // erases, syncs, persists
    });
    m_opened_files_window.SetRestartPreviewCallback([this]() {
        m_video_player.restart_hover_preview();
    });
    m_opened_files_window.SetRescanTomlCallback([this]() {
        if (!m_app_state.state_path().empty())
            LoadOpenedFilesHistoryFromToml(m_app_state.state_path());
    });
    m_opened_files_window.SetMenuShortcutsCallbacks(
        [this]() { m_open_image_dialogs.begin_open_image_dialog(); },
        [this]() { m_open_image_dialogs.open_online_popup(); },
        [this]() {
            const std::vector<std::string> open_sources = m_video_player.open_sources();
            if (open_sources.empty())
                return;
            const bool should_restore = !m_app_state.are_all_startup_videos_fixed(open_sources);
            m_app_state.set_startup_video_for_sources(open_sources, should_restore);
        },
        [this]() {
            const std::vector<std::string> open_sources = m_video_player.open_sources();
            return m_app_state.are_all_startup_videos_fixed(open_sources);
        });

    // ---- Wire MediaLoadHandler ---------------------------------------------
    // Must be called last — all subsystems must be constructed before this.
    m_load_handler.setup(
        &m_viewer,
        &m_video_player,
        &m_bulk_image_open,
        &m_history_mgr,
        &m_video_downloader,
        &m_opened_files_window,
        m_vk);
}

/**
 * @brief Shut down all GPU-owning subsystems and release libcurl.
 *
 * Must be called before ImGui_ImplVulkan_Shutdown().  After this call no
 * further rendering methods may be invoked.
 */
void MainMenuBar::Shutdown()
{
    // Nothing to clean up if Vulkan was never initialised.
    if (!m_vk)
        return;

    m_bulk_image_open.shutdown();   // stop the background path-validation thread
    m_video_player.shutdown();      // release mpv handles and Vulkan resources
    m_video_downloader.shutdown();  // cancel and join background download threads
    m_history_preview.shutdown();   // free thumbnail textures from Vulkan

    // Release all VkImage / VkSampler / VkDescriptorSet handles.
    m_viewer.shutdown(*m_vk);

    // Release the libcurl global state (matches curl_global_init in Setup).
    curl_global_cleanup();
}

// ============================================================================
// History + config forwarding — thin wrappers over MediaHistoryManager
// ============================================================================

/**
 * @brief Restore image history from persisted state and signal video restoration.
 *
 * Delegates the actual work (including title backfilling) to MediaHistoryManager.
 *
 * @param state  Previously loaded TOML state object.
 */
void MainMenuBar::ApplyHistory(const WindowStateToml &state)
{
    m_app_state.apply_history(state);
}

/**
 * @brief Restore runtime config values from persisted state.
 * @param state  Previously loaded TOML state object.
 */
void MainMenuBar::ApplyRuntimeConfig(const WindowStateToml &state)
{
    m_app_state.apply_runtime_config(state);
}

/**
 * @brief Load opened-files history directly from a TOML file at startup.
 *
 * Used when the caller wants to load history without going through the full
 * ApplyHistory / ApplyRuntimeConfig round-trip.
 *
 * @param file_path  Path to the application state TOML file.
 * @return           True if the file was read and history was applied.
 */
bool MainMenuBar::LoadOpenedFilesHistoryFromToml(const std::filesystem::path &file_path)
{
    return m_app_state.load_opened_files_history_from_toml(file_path);
}

/**
 * @brief Set the TOML file path forwarded to MediaHistoryManager.
 *
 * Must be called before the first Save operation so that persist() knows
 * where to write.
 *
 * @param file_path  Absolute path to the application state TOML file.
 */
void MainMenuBar::SetStatePath(const std::filesystem::path &file_path)
{
    m_app_state.set_state_path(file_path);
}

/**
 * @brief Serialise history into a TOML state object for the main save routine.
 * @param state  Output TOML state object; its image_history field is replaced.
 */
void MainMenuBar::ExportHistory(WindowStateToml *state)
{
    m_video_player.sync_history_state(m_history_mgr.entries());
    m_app_state.export_history(state);
}

/**
 * @brief Serialise runtime config into a TOML state object for the main save.
 * @param state  Output TOML state object.
 */
void MainMenuBar::ExportRuntimeConfig(WindowStateToml *state) const
{
    m_app_state.export_runtime_config(state);
}

/**
 * @brief Set the directory where hover-thumbnail PNGs are written.
 *
 * Also registers the "Clear Thumbnail Cache" callback with ConfigRuntime so
 * it can be triggered from the runtime settings panel.
 *
 * @param dir  Absolute path to the thumbnail cache directory.
 */
void MainMenuBar::SetThumbDir(const std::filesystem::path &dir)
{
    m_app_state.set_thumb_dir(dir);
}

/**
 * @brief Set the directory where background-downloaded video files are stored.
 *
 * Registers the "Clear Video Cache" and "Rebuild Video Cache" callbacks with
 * ConfigRuntime so they can be triggered from the runtime settings panel.
 *
 * @param dir  Absolute path to the video download cache directory.
 */
void MainMenuBar::SetDownloadCacheDir(const std::filesystem::path &dir)
{
    m_app_state.set_download_cache_dir(dir);
}

// ============================================================================
// Per-frame build
// ============================================================================

/**
 * @brief Render the main menu bar and drive all per-frame media operations.
 *
 * Frame execution order:
 *   1. evict_closed()              — free GPU resources for closed image windows.
 *   2. process_pending_save()      — finalise any deferred video save dialog.
 *   3. restore_from_history()      — one-shot startup video restoration.
 *   4. take_completed() downloads  — update cached_path for finished downloads.
 *   5. process_pending_paths()     — route file-dialog results to consumers.
 *   6. process_pending_urls()      — download/stream URL-dialog results.
 *   7. drain_bulk_queue()          — open one bulk-validated path per frame.
 *   8. Menu bar rendering          — File, View menus and their sub-menus.
 *   9. draw_url_popup()            — modal dialog for "Open Online...".
 *  10. update_frames() + draw()    — video player frame update and rendering.
 *  11. draw_windows()              — ImageViewerPanel image window rendering.
 *  12. draw() + focus dispatch     — OpenedFilesWindow rendering.
 */
void MainMenuBar::Build()
{
    // -------------------------------------------------------------------------
    // Step 1 — evict closed image windows from the GPU
    // -------------------------------------------------------------------------

    /**
     * Remove any images whose open flag was cleared by the ImGui [x] button.
     * Runs first, before any loading, so freed descriptor slots are available
     * for new uploads in the same frame.
     */
    if (m_vk)
        m_viewer.evict_closed(*m_vk);

    // -------------------------------------------------------------------------
    // Step 2 — finalise deferred video save
    // -------------------------------------------------------------------------

    // Process any pending save operation triggered by the video context menu.
    m_video_context_menu.process_pending_save();

    // -------------------------------------------------------------------------
    // Step 3 — one-shot startup video restoration
    // -------------------------------------------------------------------------

    if (m_app_state.take_restore_videos_on_startup_pending()) {
        // Rebuild video windows from the history list exactly once at startup.
        m_load_handler.restore_from_history();
    }

    // -------------------------------------------------------------------------
    // Step 4 — drain completed background downloads
    // -------------------------------------------------------------------------

    /**
     * Background downloads complete asynchronously.  Each completed result
     * carries the original URL and the local file path.  We record that path
     * in the matching history entry so it is used on the next startup.
     */
    for (const auto &result : m_video_downloader.take_completed()) {
        if (!result.ok)
            continue; // skip failed downloads silently

        // Find the history entry that matches the completed download URL.
        for (auto &h : m_history_mgr.entries()) {
            if (h.source == result.url) {
                h.cached_path = result.cached_path.string(); // record the local path
                break; // only one entry per source URL
            }
        }

        // Persist the updated cached_path immediately.
        m_history_mgr.persist();

        // Kick off seek-preview for any open window that was streaming this URL.
        m_video_player.notify_download_complete(result.url, result.cached_path);
    }

    // -------------------------------------------------------------------------
    // Steps 5 – 7 — delegate media loading to MediaLoadHandler
    // -------------------------------------------------------------------------

    // Route file-dialog results (may start a bulk-queue job for large batches).
    m_load_handler.process_pending_paths(m_open_image_dialogs.take_pending_paths());

    // Download / stream URL-popup results.
    m_load_handler.process_pending_urls(m_open_image_dialogs.take_pending_urls());

    // Open one bulk-validated path per frame to avoid descriptor-pool exhaustion.
    m_load_handler.drain_bulk_queue();

    // -------------------------------------------------------------------------
    // Step 8 — menu bar rendering
    // -------------------------------------------------------------------------

    // BeginMainMenuBar returns false when the bar is not visible (e.g. fullscreen).
    if (!ImGui::BeginMainMenuBar())
        return;

    // ---- File menu ----------------------------------------------------------

    if (ImGui::BeginMenu("File")) {
        // Trigger the native OS file-open dialog.
        if (ImGui::MenuItem("Open Image...", "Ctrl+O"))
            m_open_image_dialogs.begin_open_image_dialog();

        // Trigger the URL input modal.
        if (ImGui::MenuItem("Open Online..."))
            m_open_image_dialogs.open_online_popup();

        // ---- Recent sub-menu ------------------------------------------------

        auto &history = m_history_mgr.entries(); // mutable ref — no copy
        if (!history.empty() && ImGui::BeginMenu("Recent")) {
            const auto result = RecentHistoryMenu::draw_entries(
                history,
                {
                    .on_open = [this](WindowStateToml::ImageHistoryEntry &entry) {
                        if (entry.kind == "file")
                            m_open_image_dialogs.queue_path(entry.source);
                        else
                            m_open_image_dialogs.queue_url(entry.source);
                    },
                    .on_hover = [this](WindowStateToml::ImageHistoryEntry &entry) {
                        m_history_preview.draw_for_hover(entry);
                    },
                    .on_after_item = [this](WindowStateToml::ImageHistoryEntry &entry) {
                        const bool is_video = VideoPlayer::is_video_path(entry.source) ||
                                              VideoPlayer::is_video_url(entry.source);

                        if (is_video) {
                            if (const auto result = m_video_context_menu.draw_for_item(entry);
                                result.erase || result.restart_preview) {
                                if (result.restart_preview)
                                    m_video_player.restart_hover_preview();

                                if (result.erase) {
                                    m_history_mgr.erase(result.erase_source, m_opened_files_window);
                                    ImGui::EndMenu();
                                    ImGui::EndMenu();
                                    return true;
                                }
                            }
                            return false;
                        }

                        if (const auto erase = HistoryContextMenu::draw_for_item(entry.source)) {
                            m_history_mgr.erase(*erase, m_opened_files_window);
                            ImGui::EndMenu();
                            ImGui::EndMenu();
                            return true;
                        }

                        return false;
                    },
                });

            // Show a count of hidden entries if the history exceeds k_max_shown.
            if (result.shown < static_cast<int>(history.size())) {
                ImGui::Separator();
                ImGui::TextDisabled("(%zu more not shown)",
                                    history.size() - static_cast<size_t>(result.shown));
            }

            if (!result.stopped) {
                ImGui::Separator();
                if (ImGui::MenuItem("Clear History"))
                    m_history_mgr.clear(m_opened_files_window); // clears + persists

                ImGui::EndMenu(); // end "Recent"
            }
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Quit", "Alt+F4"))
            request_quit = true; // main loop polls this flag each iteration

        ImGui::EndMenu(); // end "File"
    }

    // ---- View menu ----------------------------------------------------------

    if (ImGui::BeginMenu("View")) {
        // Style editor toggle — only shown when a style editor is attached.
        if (m_style_editor)
            ImGui::MenuItem("Style Editor", nullptr, &m_style_editor->IsOpen);

        if (m_show_demo_window)
            ImGui::MenuItem("Demo Window", nullptr, m_show_demo_window);

        if (m_show_another_window)
            ImGui::MenuItem("Another Window", nullptr, m_show_another_window);

        ImGui::MenuItem("Opened Files", nullptr, &m_opened_files_window.IsOpen);
        ImGui::MenuItem("Runtime Config", nullptr, &m_config_runtime.IsOpen);

        // Let the image panel add one toggle item per open image window.
        if (m_viewer.count() > 0) {
            ImGui::Separator();
            m_viewer.build_view_menu_items(); // adds "re-show" items for minimised windows
        }

        ImGui::EndMenu(); // end "View"
    }

    ImGui::EndMainMenuBar();

    // -------------------------------------------------------------------------
    // Step 9 — URL input popup
    // -------------------------------------------------------------------------

    /**
     * draw_url_popup() calls ImGui::OpenPopup when m_show_url_popup is set, then
     * renders the modal.  It must be called outside of BeginMainMenuBar/EndMainMenuBar
     * so that the modal's parent is the full-screen overlay, not the menu bar.
     */
    m_open_image_dialogs.draw_url_popup();

    // -------------------------------------------------------------------------
    // Steps 10 – 12 — delegate rendering to subsystem panels
    // -------------------------------------------------------------------------

    m_video_player.update_frames(); // decode next video frame for each active player
    m_config_runtime.Draw();        // runtime settings panel (may be hidden)

    m_viewer.draw_windows();        // one ImGui window per open image
    m_video_player.draw();          // one ImGui window per open video

    // Draw the "Opened Files" companion panel and handle any activation or focus.
    int focus_id = -1;
    const auto activated = m_opened_files_window.draw(
        m_viewer, m_history_preview, &focus_id, &m_video_context_menu);

    // If a window-focus request came back, forward it to the image viewer.
    if (focus_id >= 0)
        m_viewer.request_focus(focus_id);

    // If the user activated a history entry from the list, queue it for loading.
    if (activated.has_value()) {
        if (activated->kind == "file")
            m_open_image_dialogs.queue_path(activated->source);
        else if (activated->kind == "url")
            m_open_image_dialogs.queue_url(activated->source);
    }
}