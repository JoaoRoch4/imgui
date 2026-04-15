#pragma once

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include "imgui_console.hpp"
#include "imgui_debug_log_mirror.hpp"
#include <SDL3/SDL.h>
#include <memory>
#include <string>
#include <vector>

class EmojiAtlas;

// ImGuiLayerSDLGPU3 owns the Dear ImGui context and SDL3+SDLGPU3 backend wiring.
class ImGuiLayerSDLGPU3
{
public:
    ImGuiLayerSDLGPU3();

    bool   ShowDemoWindow;
    bool   ShowAnotherWindow;
    bool   ShowDebugLogMirrorWindow;
    bool   ShowTerminalWindow;
    bool   ShowTestEngineWindow;
    bool   ShowEmojiAtlasWindow;
    bool   RequestQuit;
    ImVec4 ClearColor;

    ImGuiDebugLogMirror DebugLogMirror;

    struct TerminalTab {
        TerminalTab();
        std::string                      name;
        std::unique_ptr<ConsoleCommands> console;
        bool                             open;
    };
    std::vector<TerminalTab> Terminals;

    void AddTerminal(const char* name = nullptr);

    void Init(SDL_Window* window, ImGui_ImplSDLGPU3_InitInfo& init_info, float main_scale);
    void Shutdown();
    void ProcessEvent(const SDL_Event* event);
    void NewFrame();
    void BuildUI();
    void SetEmojiAtlas(const EmojiAtlas* atlas);
    void Render();

private:
    void WireTerminalCallbacks(ConsoleCommands& c);
    void DrawTerminals();
    void DrawEmojiAtlasWindow();

    const EmojiAtlas* EmojiAtlasView { nullptr };
};
