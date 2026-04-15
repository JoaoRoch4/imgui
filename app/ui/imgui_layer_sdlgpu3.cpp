#include "imgui_layer_sdlgpu3.hpp"

#include "emoji_atlas.hpp"
#include "imgui_freetype.h"

#include <array>
#include <format>
#include <string>

ImGuiLayerSDLGPU3::ImGuiLayerSDLGPU3()
    : ShowDemoWindow{true}
    , ShowAnotherWindow{false}
    , ShowDebugLogMirrorWindow{true}
    , ShowTerminalWindow{true}
    , ShowTestEngineWindow{true}
    , ShowEmojiAtlasWindow{true}
    , RequestQuit{false}
    , ClearColor{0.45f, 0.55f, 0.60f, 1.00f}
{}

ImGuiLayerSDLGPU3::TerminalTab::TerminalTab()
    : open{true}
{}

void ImGuiLayerSDLGPU3::Init(SDL_Window* window, ImGui_ImplSDLGPU3_InitInfo& init_info, float main_scale)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_Init(&init_info);

    DebugLogMirror.Open("/tmp/imgui_debug.log");
    AddTerminal("Terminal 1");

    style.FontSizeBase = 20.0f;
    io.Fonts->AddFontFromFileTTF("/usr/share/fonts/google-noto/NotoSans-Regular.ttf");
    {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(
            "/home/joao/.local/share/fonts/google/NotoSansSC[wght].ttf",
            0.0f, &cfg);
    }
    // Merge Arabic (Arabic script + Presentation Forms U+FE70-U+FEFF).
    {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(
            "/home/joao/.local/share/fonts/google/NotoSansArabic[wdth,wght].ttf",
            0.0f, &cfg);
    }
    // Merge Mathematical Alphanumeric Symbols (U+1D400-U+1D7FF): Fraktur, Script, etc.
    {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(
            "/home/joao/.local/share/fonts/google/NotoSansMath-Regular.ttf",
            0.0f, &cfg);
    }
    // Merge Symbols 2 — rare/historic scripts, ꙮ, etc.
    {
        ImFontConfig cfg;
        cfg.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(
            "/home/joao/.local/share/fonts/google/NotoSansSymbols2-Regular.ttf",
            0.0f, &cfg);
    }
    // Merge color emoji — COLRv1/SVG via PlutoSVG.
    {
        ImFontConfig cfg;
        cfg.MergeMode       = true;
        cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LoadColor;
        io.Fonts->AddFontFromFileTTF(
            "/home/joao/.local/share/fonts/google/NotoColorEmoji-Regular.ttf",
            0.0f, &cfg);
    }
}

void ImGuiLayerSDLGPU3::WireTerminalCallbacks(ConsoleCommands& c)
{
    c.OnDemoToggle  = [this](bool show) { ShowDemoWindow = show; };
    c.OnStyleChange = [](int) { /* style already applied in-place by CmdStyle */ };
    c.OnQuit        = [this]() { RequestQuit = true; };
}

void ImGuiLayerSDLGPU3::AddTerminal(const char* name)
{
    TerminalTab t;
    t.name    = name ? name : ("Terminal " + std::to_string(Terminals.size() + 1));
    t.console = std::make_unique<ConsoleCommands>();
    t.console->SetEmojiAtlas(EmojiAtlasView);
    WireTerminalCallbacks(*t.console);
    Terminals.push_back(std::move(t));
}

void ImGuiLayerSDLGPU3::SetEmojiAtlas(const EmojiAtlas* atlas)
{
    EmojiAtlasView = atlas;
    for (TerminalTab& terminal : Terminals)
        terminal.console->SetEmojiAtlas(atlas);
}

void ImGuiLayerSDLGPU3::DrawTerminals()
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
            TerminalTab& t = Terminals.at(i);
            bool open = t.open;
            std::string label = std::format("{}##tab{}", t.name, i);
            if (ImGui::BeginTabItem(label.c_str(), &open))
            {
                std::string id = std::format("{}", i);
                t.console->DrawContents(id.c_str());
                ImGui::EndTabItem();
            }
            t.open = open;
            if (!open)
                Terminals.erase(Terminals.begin() + i);
            else
                ++i;
        }
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing))
            AddTerminal();
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void ImGuiLayerSDLGPU3::DrawEmojiAtlasWindow()
{
    if (EmojiAtlasView == nullptr)
        return;

    ImGui::SetNextWindowSize(ImVec2(420.0f, 260.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Emoji Atlas", &ShowEmojiAtlasWindow))
    {
        ImGui::End();
        return;
    }

    ImGui::Text("Merged text path:");
    ImGui::TextUnformatted("\xF0\x9F\x8C\x9F  \xF0\x9F\x9A\x80");
    ImGui::Separator();

    struct EmojiSample {
        ImWchar     codepoint;
        const char* label;
    };
    const std::array<EmojiSample, 2> samples {{
        { static_cast<ImWchar>(0x1F31F), "U+1F31F glowing star" },
        { static_cast<ImWchar>(0x1F680), "U+1F680 rocket" },
    }};

    for (const EmojiSample& sample : samples)
    {
        ImGui::TextUnformatted(sample.label);
        const EmojiAtlas::GlyphEntry* glyph = EmojiAtlasView->LookupGlyph(sample.codepoint);
        if (glyph == nullptr)
        {
            ImGui::TextUnformatted("Atlas glyph missing");
            continue;
        }

        const ImVec2 image_size {
            static_cast<float>(glyph->RenderW) * 2.0f,
            static_cast<float>(glyph->RenderH) * 2.0f,
        };
        ImGui::Image(EmojiAtlasView->GetTextureRef(), image_size,
                     ImVec2(glyph->U0, glyph->V0), ImVec2(glyph->U1, glyph->V1));
        ImGui::SameLine();
        ImGui::Text("%dx%d", glyph->RenderW, glyph->RenderH);
    }

    ImGui::Separator();
    ImGui::Text("Atlas size: %d x %d", EmojiAtlasView->AtlasWidth(), EmojiAtlasView->AtlasHeight());
    ImGui::End();
}

void ImGuiLayerSDLGPU3::Shutdown()
{
    DebugLogMirror.Close();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void ImGuiLayerSDLGPU3::ProcessEvent(const SDL_Event* event)
{
    ImGui_ImplSDL3_ProcessEvent(event);
}

void ImGuiLayerSDLGPU3::NewFrame()
{
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    DebugLogMirror.Tick();
}

void ImGuiLayerSDLGPU3::BuildUI()
{
    if (ShowDemoWindow)
        ImGui::ShowDemoWindow(&ShowDemoWindow);

    {
        static float f       = 0.0f;
        static int   counter = 0;

        ImGui::Begin("Hello, world!");
        ImGui::Text("This is some useful text.");
        ImGui::Checkbox("Demo Window", &ShowDemoWindow);
        ImGui::Checkbox("Another Window", &ShowAnotherWindow);
        ImGui::Checkbox("Debug Log", &ShowDebugLogMirrorWindow);
        ImGui::Checkbox("Terminals", &ShowTerminalWindow);
        ImGui::Checkbox("Test Engine", &ShowTestEngineWindow);
        ImGui::Checkbox("Emoji Atlas", &ShowEmojiAtlasWindow);
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

    if (ShowDebugLogMirrorWindow)
        DebugLogMirror.ShowWindow(&ShowDebugLogMirrorWindow);

    if (ShowTerminalWindow)
        DrawTerminals();

    if (ShowEmojiAtlasWindow)
        DrawEmojiAtlasWindow();

    if (ShowAnotherWindow)
    {
        ImGui::Begin("Another Window", &ShowAnotherWindow);
        ImGui::Text("Hello from another window!");
        if (ImGui::Button("Close Me"))
            ShowAnotherWindow = false;
        ImGui::End();
    }
}

void ImGuiLayerSDLGPU3::Render()
{
    ImGui::Render();
}
