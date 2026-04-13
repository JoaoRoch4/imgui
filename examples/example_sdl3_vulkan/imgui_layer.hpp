#pragma once

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"
#include "imgui_debug_log_mirror.hpp"
#include "imgui_console.hpp"
#include <SDL3/SDL.h>
#include <memory>
#include <string>
#include <vector>

// ImGuiLayer owns the Dear ImGui context, both backends, and all UI window logic.
class ImGuiLayer
{
public:
    ImGuiLayer();

    // State exposed to the render loop (clear colour is passed to VulkanContext).
    bool   ShowDemoWindow;
    bool   ShowAnotherWindow;
    bool   ShowDebugLogMirrorWindow;
    bool   ShowTerminalWindow;
    bool   ShowTestEngineWindow;
    bool   RequestQuit;
    ImVec4 ClearColor;

    // Realtime file mirror of the Dear ImGui debug log (tail -f friendly).
    ImGuiDebugLogMirror DebugLogMirror;

    // Terminal tabs — each entry owns an independent ConsoleCommands instance.
    struct TerminalTab {
        TerminalTab();
        std::string                      name;
        std::unique_ptr<ConsoleCommands> console;
        bool                             open;
    };
    std::vector<TerminalTab> Terminals;

    // Append a new terminal tab.  If name is null, auto-generates "Terminal N".
    void AddTerminal(const char* name = nullptr);

    // Create the ImGui context, configure IO flags, apply style, init both backends,
    // load fonts.  init_info must be fully populated (see VulkanContext::MakeInitInfo).
    void Init(SDL_Window* window, ImGui_ImplVulkan_InitInfo& init_info, float main_scale);

    // Shutdown both backends and destroy the ImGui context.
    void Shutdown();

    // Forward an SDL event to the SDL3 backend (call for every polled event).
    void ProcessEvent(const SDL_Event* event);

    // Call ImGui_ImplVulkan_NewFrame + ImGui_ImplSDL3_NewFrame + ImGui::NewFrame.
    void NewFrame();

    // Build all UI windows for the current frame.
    void BuildUI();

    // Call ImGui::Render() to finalise draw data.
    void Render();

private:
    void WireTerminalCallbacks(ConsoleCommands& c);
    void DrawTerminals();
};
