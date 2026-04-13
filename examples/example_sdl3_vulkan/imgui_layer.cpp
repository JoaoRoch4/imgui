#include "imgui_layer.hpp"
#include "../../misc/freetype/imgui_freetype.h"

void ImGuiLayer::Init(SDL_Window* window, ImGui_ImplVulkan_InitInfo& init_info, float main_scale)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

    // Bake a fixed style / DPI scale (see docs/FONTS.md for dynamic per-monitor scaling).
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    ImGui_ImplSDL3_InitForVulkan(window);
    ImGui_ImplVulkan_Init(&init_info);

    // Start mirroring the debug log to a file (follow with: tail -f /tmp/imgui_debug.log)
    DebugLogMirror.Open("/tmp/imgui_debug.log");

    // Create the initial terminal tab (wires all callbacks internally).
    AddTerminal("Terminal 1");
    // Uncomment to enable specific log categories, e.g.:
    // ImGuiDebugLogMirror::SetFlags(ImGuiDebugLogFlags_EventError | ImGuiDebugLogFlags_EventActiveId);

    // Load fonts — see docs/FONTS.md.
    // Using 1.92 dynamic font system (no glyph ranges required).
    // Prerequisites already enabled in imconfig.h:
    //   IMGUI_USE_WCHAR32                  — codepoints > 0xFFFF (emoji)
    //   IMGUI_ENABLE_FREETYPE              — FreeType rasterizer
    //   IMGUI_ENABLE_FREETYPE_PLUTOSVG     — COLRv1 color emoji via PlutoSVG
    style.FontSizeBase = 20.0f;

    // 1. Primary: NotoSans — solid Latin, Greek, Cyrillic, Arabic, Hebrew …
    io.Fonts->AddFontFromFileTTF("/usr/share/fonts/google-noto/NotoSans-Regular.ttf");

    // 2. Merge CJK (Chinese, Japanese, Korean) into the same font slot.
    {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(
            "/home/joao/.local/share/fonts/google/NotoSansSC[wght].ttf",
            0.0f, &cfg);
    }

    // 3. Merge color emoji (UTF-16 / surrogate-pair range, i.e. U+1F000…U+1FFFF).
    //    Requires IMGUI_USE_WCHAR32 + IMGUI_ENABLE_FREETYPE + LoadColor.
    //    PlutoSVG (IMGUI_ENABLE_FREETYPE_PLUTOSVG) handles COLRv1 outlines.
    {
        ImFontConfig cfg;
        cfg.MergeMode       = true;
        cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LoadColor;
        io.Fonts->AddFontFromFileTTF(
            "/home/joao/.local/share/fonts/google/NotoColorEmoji-Regular.ttf",
            0.0f, &cfg);
    }
}

void ImGuiLayer::WireTerminalCallbacks(ConsoleCommands& c)
{
    c.OnDemoToggle  = [this](bool show) { ShowDemoWindow = show; };
    c.OnStyleChange = [](int) { /* style already applied in-place by CmdStyle */ };
    c.OnQuit        = [this]()           { RequestQuit = true; };
}

void ImGuiLayer::AddTerminal(const char* name)
{
    TerminalTab t;
    t.name    = name ? name : ("Terminal " + std::to_string(Terminals.size() + 1));
    t.console = std::make_unique<ConsoleCommands>();
    WireTerminalCallbacks(*t.console);
    Terminals.push_back(std::move(t));
}

void ImGuiLayer::DrawTerminals()
{
    ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Terminals", &ShowTerminalWindow))
    {
        ImGui::End();
        return;
    }
    if (ImGui::BeginTabBar("##termtabs"))
    {
        for (int i = 0; i < static_cast<int>(Terminals.size()); )
        {
            TerminalTab& t = Terminals[i];
            bool open = t.open;
            char label[64];
            std::snprintf(label, sizeof(label), "%s##tab%d", t.name.c_str(), i);
            if (ImGui::BeginTabItem(label, &open))
            {
                char id[16];
                std::snprintf(id, sizeof(id), "%d", i);
                t.console->DrawContents(id);
                ImGui::EndTabItem();
            }
            t.open = open;
            if (!open)
                Terminals.erase(Terminals.begin() + i);
            else
                ++i;
        }
        // "+" button appends a new terminal tab.
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing))
            AddTerminal();
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void ImGuiLayer::Shutdown()
{
    DebugLogMirror.Close();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiLayer::ProcessEvent(const SDL_Event* event)
{
    ImGui_ImplSDL3_ProcessEvent(event);
}

void ImGuiLayer::NewFrame()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    DebugLogMirror.Tick();  // append any new debug-log bytes to /tmp/imgui_debug.log
}

void ImGuiLayer::BuildUI()
{
    // 1. Show the big demo window.
    if (ShowDemoWindow)
        ImGui::ShowDemoWindow(&ShowDemoWindow);

    // 2. Show a simple window that we create ourselves.
    {
        static float f       = 0.0f;
        static int   counter = 0;

        ImGui::Begin("Hello, world!");
        ImGui::Text("This is some useful text.");
        ImGui::Checkbox("Demo Window",    &ShowDemoWindow);
        ImGui::Checkbox("Another Window", &ShowAnotherWindow);
        ImGui::Checkbox("Debug Log",      &ShowDebugLogMirrorWindow);
        ImGui::Checkbox("Terminals",        &ShowTerminalWindow);
        ImGui::SliderFloat("float", &f, 0.0f, 1.0f);
        ImGui::ColorEdit3("clear color", &ClearColor.x);
        if (ImGui::Button("Button"))
            counter++;
        ImGui::SameLine();
        ImGui::Text("counter = %d", counter);
        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)",
                    1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
        ImGui::End();
    }

    // 3. Debug log mirror window (toggle with the "Debug Log" button below).
    if (ShowDebugLogMirrorWindow)
        DebugLogMirror.ShowWindow(&ShowDebugLogMirrorWindow);

    // 4. Terminal window.
    if (ShowTerminalWindow)
        DrawTerminals();

    // 5. Show another simple window.
    if (ShowAnotherWindow)
    {
        ImGui::Begin("Another Window", &ShowAnotherWindow);
        ImGui::Text("Hello from another window!");
        if (ImGui::Button("Close Me"))
            ShowAnotherWindow = false;
        ImGui::End();
    }
}

void ImGuiLayer::Render()
{
    ImGui::Render();
}
