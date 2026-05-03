#include "video_context_menu.hpp"

#include "core/log/debug_log.hpp"
#include "image_downloader.hpp"
#include "video_player.hpp"

#include <SDL3/SDL_dialog.h>
#include <imgui.h>


// ---------------------------------------------------------------------------
// Constructor / setup
// ---------------------------------------------------------------------------

VideoContextMenu::VideoContextMenu()
    : m_window{nullptr}
    , m_copy_source{}
    , m_copy_history_source{}
    , m_copy_dest{}
    , m_on_save_success{}
    , m_can_toggle_hwdec{}
    , m_is_hwdec_enabled{}
    , m_on_toggle_hwdec{}
{
}

void VideoContextMenu::setup(SDL_Window *window)
{
    m_window = window;
}

void VideoContextMenu::set_on_save_success(
    std::function<void(const std::string &, const std::filesystem::path &)> cb)
{
    m_on_save_success = std::move(cb);
}

void VideoContextMenu::set_hwdec_callbacks(std::function<bool(const std::string &)> can_toggle,
                                           std::function<bool(const std::string &)> is_enabled,
                                           std::function<void(const std::string &)> toggle)
{
    m_can_toggle_hwdec = std::move(can_toggle);
    m_is_hwdec_enabled = std::move(is_enabled);
    m_on_toggle_hwdec = std::move(toggle);
}

bool VideoContextMenu::can_toggle_hwdec(const std::string &source) const
{
    return m_can_toggle_hwdec && m_can_toggle_hwdec(source);
}

bool VideoContextMenu::is_hwdec_enabled(const std::string &source) const
{
    return m_is_hwdec_enabled && m_is_hwdec_enabled(source);
}

void VideoContextMenu::toggle_hwdec(const std::string &source) const
{
    if (m_on_toggle_hwdec)
        m_on_toggle_hwdec(source);
}

// ---------------------------------------------------------------------------
// Save-file dialog callback (called on the main thread by SDL3)
// ---------------------------------------------------------------------------

void VideoContextMenu::save_dialog_callback(void *userdata,
                                             const char *const *filelist,
                                             int /*filter*/)
{
    auto *self = static_cast<VideoContextMenu *>(userdata);
    if (!filelist || !filelist[0]) {
        self->m_copy_source.clear(); // user cancelled
        self->m_copy_history_source.clear();
        return;
    }
    self->m_copy_dest = std::filesystem::path(filelist[0]);
}

// ---------------------------------------------------------------------------
// Shared menu body (used by both draw_for_item and draw_for_window)
// ---------------------------------------------------------------------------

static VideoContextMenu::Result draw_menu_body(
    VideoContextMenu *self,
    SDL_Window *window,
    std::filesystem::path &copy_source,
    std::string &copy_history_source,
    std::filesystem::path &copy_dest,
    const WindowStateToml::ImageHistoryEntry &entry)
{
    VideoContextMenu::Result result;

    // Short preview label.
    const std::string preview = entry.source.size() > 64
        ? entry.source.substr(0, 61) + "..."
        : entry.source;
    ImGui::TextDisabled("%s", preview.c_str());
    ImGui::Separator();

    // ----- Remove from History --------------------------------------------
    if (ImGui::MenuItem("Remove from History")) {
        result.erase        = true;
        result.erase_source = entry.source;
    }

    // ----- Save Video As… ------------------------------------------------
    std::filesystem::path save_src;
    std::error_code ec;
    if (!entry.cached_path.empty()) {
        const std::filesystem::path cp(entry.cached_path);
        if (std::filesystem::exists(cp, ec))
            save_src = cp;
    }
    if (save_src.empty()) {
        const std::filesystem::path sp(entry.source);
        if (std::filesystem::exists(sp, ec) && VideoPlayer::is_video_path(sp))
            save_src = sp;
    }

    const bool can_save = !save_src.empty();
    if (!can_save)
        ImGui::BeginDisabled();

    if (ImGui::MenuItem("Save Video As\xe2\x80\xa6")) {
        std::string suggested = ImageDownloader::title_from_url(entry.source);
        if (suggested.empty())
            suggested = save_src.filename().string();

        const std::filesystem::path suggested_path(suggested);
        if (!save_src.extension().empty() && suggested_path.extension().empty())
            suggested += save_src.extension().string();

        static const SDL_DialogFileFilter filters[] = {
            {"Video files", "mp4;mkv;avi;mov;webm;flv;wmv;m4v"},
            {"All files",   "*"},
        };
        copy_source = save_src;
        copy_history_source = entry.source;
        copy_dest.clear();
        SDL_ShowSaveFileDialog(VideoContextMenu::save_dialog_callback,
                               self, window, filters, 2, suggested.c_str());
    }

    if (!can_save)
        ImGui::EndDisabled();

    const bool can_toggle_hwdec = self->can_toggle_hwdec(entry.source);
    if (!can_toggle_hwdec)
        ImGui::BeginDisabled();

    const bool hwdec_enabled = self->is_hwdec_enabled(entry.source);
    if (ImGui::MenuItem("Use Hardware Decode", nullptr, hwdec_enabled)) {
        result.toggle_hwdec = true;
        self->toggle_hwdec(entry.source);
    }

    if (!can_toggle_hwdec)
        ImGui::EndDisabled();

    ImGui::Separator();
    if (ImGui::MenuItem("Restart Preview Thread"))
        result.restart_preview = true;

    return result;
}

VideoContextMenu::Result VideoContextMenu::draw_menu_items(
    const WindowStateToml::ImageHistoryEntry &entry)
{
    return draw_menu_body(this, m_window, m_copy_source, m_copy_history_source, m_copy_dest, entry);
}

// ---------------------------------------------------------------------------
// draw_for_item
// ---------------------------------------------------------------------------

VideoContextMenu::Result VideoContextMenu::draw_for_item(
    const WindowStateToml::ImageHistoryEntry &entry)
{
    Result result;
    if (!ImGui::BeginPopupContextItem())
        return result;
    result = draw_menu_body(this, m_window, m_copy_source, m_copy_history_source, m_copy_dest, entry);
    ImGui::EndPopup();
    return result;
}

// ---------------------------------------------------------------------------
// draw_for_window
// ---------------------------------------------------------------------------

VideoContextMenu::Result VideoContextMenu::draw_for_window(
    const WindowStateToml::ImageHistoryEntry &entry,
    const char *popup_id)
{
    Result result;
    if (!ImGui::BeginPopupContextWindow(popup_id))
        return result;
    result = draw_menu_body(this, m_window, m_copy_source, m_copy_history_source, m_copy_dest, entry);
    ImGui::EndPopup();
    return result;
}

// ---------------------------------------------------------------------------
// process_pending_save
// ---------------------------------------------------------------------------

void VideoContextMenu::process_pending_save()
{
    if (m_copy_dest.empty())
        return;

    const auto dest   = m_copy_dest;
    const auto source = m_copy_source;
    const auto history_source = m_copy_history_source;
    m_copy_dest.clear();
    m_copy_source.clear();
    m_copy_history_source.clear();

    if (source.empty()) {
        APP_DEBUG_LOG("[VideoContextMenu] save cancelled (no source)");
        return;
    }

    std::error_code ec;
    std::filesystem::copy_file(source, dest,
                               std::filesystem::copy_options::overwrite_existing,
                               ec);
    if (ec)
        APP_DEBUG_LOG("[VideoContextMenu] save failed {}: {}", dest.string(), ec.message());
    else {
        APP_DEBUG_LOG("[VideoContextMenu] saved: {}", dest.string());
        if (m_on_save_success && !history_source.empty())
            m_on_save_success(history_source, dest);
    }
}
