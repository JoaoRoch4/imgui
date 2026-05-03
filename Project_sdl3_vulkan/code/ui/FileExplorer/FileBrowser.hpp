#pragma once

#include "pch.hpp"

#ifndef IMGUI_VERSION
#   error "include imgui.h before this header"
#endif

using ImGuiFileBrowserFlags = std::uint32_t;

enum ImGuiFileBrowserFlags_ : std::uint32_t
{
    ImGuiFileBrowserFlags_SelectDirectory       = 1 << 0,  // select directory instead of regular file
    ImGuiFileBrowserFlags_EnterNewFilename      = 1 << 1,  // allow user to enter new filename when selecting regular file
    ImGuiFileBrowserFlags_NoModal               = 1 << 2,  // file browsing window is modal by default. specify this to use a popup window
    ImGuiFileBrowserFlags_NoTitleBar            = 1 << 3,  // hide window title bar
    ImGuiFileBrowserFlags_NoStatusBar           = 1 << 4,  // hide status bar at the bottom of browsing window
    ImGuiFileBrowserFlags_CloseOnEsc            = 1 << 5,  // close file browser when pressing 'ESC'
    ImGuiFileBrowserFlags_CreateNewDir          = 1 << 6,  // allow user to create new directory
    ImGuiFileBrowserFlags_MultipleSelection     = 1 << 7,  // allow user to select multiple files. this will hide ImGuiFileBrowserFlags_EnterNewFilename
    ImGuiFileBrowserFlags_HideRegularFiles      = 1 << 8,  // hide regular files when ImGuiFileBrowserFlags_SelectDirectory is enabled
    ImGuiFileBrowserFlags_ConfirmOnEnter        = 1 << 9,  // confirm selection when pressing 'ENTER'
    ImGuiFileBrowserFlags_SkipItemsCausingError = 1 << 10, // when entering a new directory, any error will interrupt the process, causing the file browser to fall back to the working directory.
                                                           // with this flag, if an error is caused by a specific item in the directory, that item will be skipped, allowing the process to continue.
    ImGuiFileBrowserFlags_EditPathString        = 1 << 11, // allow user to directly edit the whole path string
};

namespace ImGui
{
    class FileBrowser
    {
    public:

        explicit FileBrowser(
            ImGuiFileBrowserFlags flags = 0,
            std::filesystem::path defaultDirectory = std::filesystem::current_path());

        FileBrowser(const FileBrowser &copyFrom);

        FileBrowser &operator=(const FileBrowser &copyFrom);

        // set the window position (in pixels)
        // default is centered
        void SetWindowPos(int posX, int posY) noexcept;

        // set the window size (in pixels)
        // default is (700, 450)
        void SetWindowSize(int width, int height) noexcept;

        // set the window title text
        void SetTitle(std::string title);

        // open the browsing window
        void Open();

        // close the browsing window
        void Close();

        // the browsing window is opened or not
        [[nodiscard]] bool IsOpened() const noexcept;

        // display the browsing window if opened
        void Display();

        // returns true when there is a selected filename
        [[nodiscard]] bool HasSelected() const noexcept;

        // set current browsing directory
        bool SetDirectory(const std::filesystem::path &dir = std::filesystem::current_path());

        // legacy interface. use SetDirectory instead.
        bool SetPwd(const std::filesystem::path &dir = std::filesystem::current_path())
        {
            return SetDirectory(dir);
        }

        // get current browsing directory
        [[nodiscard]] const std::filesystem::path &GetDirectory() const noexcept;

        // legacy interface. use GetDirectory instead.
        [[nodiscard]] const std::filesystem::path &GetPwd() const noexcept
        {
            return GetDirectory();
        }

        // returns selected filename. make sense only when HasSelected returns true
        // when ImGuiFileBrowserFlags_MultipleSelection is enabled, only one of
        // selected filename will be returned
        [[nodiscard]] std::filesystem::path GetSelected() const;

        // returns all selected filenames.
        // when ImGuiFileBrowserFlags_MultipleSelection is enabled, use this
        // instead of GetSelected
        [[nodiscard]] std::vector<std::filesystem::path> GetMultiSelected() const;

        // set selected filename to empty
        void ClearSelected();

        // (optional) set file type filters. eg. { ".h", ".cpp", ".hpp" }
        // ".*" matches any file types
        void SetTypeFilters(const std::vector<std::string> &typeFilters);

        // set currently applied type filter
        // default value is 0 (the first type filter)
        void SetCurrentTypeFilterIndex(int index);

        // set/get current file sort mode by index:
        // 0=NameAsc, 1=NameDesc, 2=Type, 3=SizeDesc, 4=DateDesc
        void SetSortModeIndex(int index);
        [[nodiscard]] int GetSortModeIndex() const noexcept;

        // set/get recent directories shown by the explorer combo.
        void SetRecentDirectories(const std::vector<std::filesystem::path> &directories);
        [[nodiscard]] std::vector<std::filesystem::path> GetRecentDirectories() const;

        // when ImGuiFileBrowserFlags_EnterNewFilename is set
        // this function will pre-fill the input dialog with a filename.
        void SetInputName(std::string_view input);

        // set a callback invoked each frame a non-directory file item is hovered.
        // the argument is the full absolute path of the hovered file.
        void SetHoverFileCallback(std::function<void(const std::filesystem::path &)> cb);

        // enable/disable the hover preview (also togglable via the in-UI checkbox).
        void SetPreviewEnabled(bool enabled) noexcept;
        [[nodiscard]] bool IsPreviewEnabled() const noexcept;

    private:

        template <class Functor>
        struct ScopeGuard
        {
            explicit ScopeGuard(Functor&& t) : func(std::move(t)) { }

            ~ScopeGuard() { func(); }

        private:

            Functor func;
        };

        enum class SortMode { NameAsc, NameDesc, Type, SizeDesc, DateDesc };

        struct FileRecord
        {
            bool                  isDir = false;
            std::filesystem::path name;
            std::string           showName;
            std::filesystem::path extension;
            std::uintmax_t        size          = 0;
            std::filesystem::file_time_type lastWriteTime = {};
        };

        static std::string ToLower(const std::string &s);

        void TouchRecentDirectory(const std::filesystem::path &dir);

        void ToolTip(const std::string_view &s);

        void UpdateFileRecords();

        void SetCurrentDirectoryUncatched(const std::filesystem::path &pwd);

        bool SetCurrentDirectoryInternal(
            const std::filesystem::path &dir,
            const std::filesystem::path &preferredFallback);

        [[nodiscard]] bool IsExtensionMatched(const std::filesystem::path &extension) const;

        void ClearRangeSelectionState();

        static void AssignToArrayStyleString(std::vector<char> &arr, std::string_view content);

        static int ExpandInputBuffer(ImGuiInputTextCallbackData *callbackData);

#ifdef _WIN32
        static std::uint32_t GetDrivesBitMask();
#endif

        // for c++17 compatibility

#if defined(__cpp_lib_char8_t)
        static std::string u8StrToStr(std::u8string s);
#endif
        static std::string u8StrToStr(std::string s);

        static std::filesystem::path u8StrToPath(const char *str);

        int width_;
        int height_;
        int posX_;
        int posY_;
        ImGuiFileBrowserFlags flags_;
        std::filesystem::path defaultDirectory_;

        std::string title_;
        std::string openLabel_;

        bool shouldOpen_;
        bool shouldClose_;
        bool isOpened_;
        bool isOk_;
        bool isPosSet_;

        std::string statusStr_;

        std::vector<std::string> typeFilters_;
        unsigned int             typeFilterIndex_;
        bool                     hasAllFilter_;

        std::filesystem::path   currentDirectory_;
        std::vector<FileRecord> fileRecords_;

        unsigned int                    rangeSelectionStart_; // enable range selection when shift is pressed
        std::set<std::filesystem::path> selectedFilenames_;

        std::string       openNewDirLabel_;
        std::vector<char> newDirNameBuffer_;
        std::vector<char> inputNameBuffer_;
        std::string       customizedInputName_;

        bool              editDir_;
        bool              setFocusToEditDir_;
        std::vector<char> currDirBuffer_;
        std::function<void(const std::filesystem::path &)> hoverFileCallback_;
        SortMode sortMode_ = SortMode::NameAsc;
        std::string searchStr_;
        std::vector<std::filesystem::path> recentDirectories_;
        bool previewEnabled_ = true;

#ifdef _WIN32
        std::uint32_t drives_;
#endif
    };
} // namespace ImGui
