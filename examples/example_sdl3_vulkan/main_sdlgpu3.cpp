// Dear ImGui: SDL3 + SDLGPU3 variant using shared app/ui and app/platform layers.

#include "app/platform/sdl_window.hpp"
#include "app/renderer/sdlgpu3/sdlgpu3_emoji_atlas.hpp"
#include "app/renderer/sdlgpu3/sdlgpu3_context.hpp"
#include "app/ui/imgui_layer_sdlgpu3.hpp"
#include "app/ui/test_engine_layer.hpp"
#include <SDL3/SDL.h>

#include <print>
#include <vector>

int main(int, char**)
{
    SDLWindow sdl;
    if (!sdl.Init("Dear ImGui SDL3+SDLGPU3 example", 1280, 800))
        return 1;

    SDLGPU3Context gpu;
    if (!gpu.Setup(sdl.Window))
        return 1;

    sdl.Show();

    ImGui_ImplSDLGPU3_InitInfo init_info = gpu.MakeInitInfo();
    SDLGPU3EmojiAtlas emoji_atlas(gpu);
    ImGuiLayerSDLGPU3 imgui;
    imgui.Init(sdl.Window, init_info, sdl.MainScale);
    const std::vector<ImWchar> emoji_codepoints {
        static_cast<ImWchar>(0x1F31F),
        static_cast<ImWchar>(0x1F680),
    };
    const std::vector<std::string> emoji_sequences {
        "👨‍👩‍👧‍👦", "🏳️‍🌈", "👩🏾‍🚀", "🧑🏻‍🤝‍🧑🏿", "🧗🏻‍♂️", "🚵‍♀️", "🧜‍♂️",
        "🧟‍♀️", "🧚‍♂️", "🧛🏾‍♂️", "🦹‍♂️", "🧝‍♀️", "🧙‍♂️", "🧞‍♂️", "🧖‍♂️",
        "🛀🏿", "🧘🏽‍♀️", "🏃‍♂️", "🏃‍♀️", "👯‍♂️", "💃🏽", "🕺🏻", "🏇🏿",
        "🧗🏼‍♂️", "🚴‍♂️", "🤽🏼‍♀️", "🤾‍♂️", "🤹🏿‍♂️",
    };
    if (emoji_atlas.Build("/usr/share/fonts/google-noto-color-emoji-fonts/Noto-COLRv1.ttf",
            32.0f, emoji_codepoints, emoji_sequences)) {
        emoji_atlas.DumpAtlasToPng("/tmp/emoji_atlas_sdlgpu3.png");
        imgui.SetEmojiAtlas(&emoji_atlas);
    } else {
        std::println(stderr, "[main_sdlgpu3] SDLGPU3 emoji atlas build failed");
    }

    TestEngineLayer test_engine;
    test_engine.Init();

    {
        ImGuiTestEngineIO& test_io = ImGuiTestEngine_GetIO(test_engine.Engine);
        test_io.ScreenCaptureFunc = [](ImGuiID, int x, int y, int w, int h,
                                       unsigned int* pixels, void* user_data) -> bool {
            return static_cast<SDLGPU3Context*>(user_data)->ReadPixels(x, y, w, h, pixels);
        };
        test_io.ScreenCaptureUserData = &gpu;
    }

    bool done = false;
    while (!done)
    {
        sdl.PollEvents(done, [&](const SDL_Event* event) { imgui.ProcessEvent(event); });
        done = done || imgui.RequestQuit;
        if (sdl.IsMinimized())
        {
            SDL_Delay(10);
            continue;
        }

        imgui.NewFrame();
        imgui.BuildUI();
        test_engine.BuildUI(&imgui.ShowTestEngineWindow);
        imgui.Render();

        ImDrawData* draw_data = ImGui::GetDrawData();
        gpu.FrameRender(draw_data, imgui.ClearColor);
        test_engine.PostSwap();
    }

    gpu.WaitIdle();
    test_engine.Stop();
    imgui.SetEmojiAtlas(nullptr);
    emoji_atlas.Cleanup();
    imgui.Shutdown();
    test_engine.Shutdown();
    gpu.Cleanup();
    sdl.Shutdown();
    return 0;
}
