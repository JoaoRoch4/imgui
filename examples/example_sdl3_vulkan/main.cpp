// Dear ImGui: standalone example application for SDL3 + Vulkan
// Learn about Dear ImGui: https://dearimgui.com/getting-started

#include "app/ui/imgui_layer.hpp"
#include "app/platform/sdl_window.hpp"
#include "app/ui/test_engine_layer.hpp"
#include "app/renderer/vulkan/vulkan_emoji_atlas.hpp"
#include "app/renderer/vulkan/vulkan_context.hpp"
#include <SDL3/SDL.h>

#include <print>
#include <vector>

#ifdef __EMSCRIPTEN__
#include "../libs/emscripten/emscripten_mainloop_stub.h"
#endif

int main(int, char**)
{
	// ── SDL + window ──────────────────────────────────────────────────────────
	SDLWindow sdl;
	if (!sdl.Init("Dear ImGui SDL3+Vulkan example", 1280, 800))
		return 1;

	// ── Vulkan ────────────────────────────────────────────────────────────────
	VulkanContext vulkan;
	vulkan.Setup(sdl.GetVulkanExtensions());

	VkSurfaceKHR surface = sdl.CreateVulkanSurface(vulkan.Instance, vulkan.Allocator);
	if (surface == VK_NULL_HANDLE)
		return 1;

	int w, h;
	sdl.GetSize(w, h);
	vulkan.SetupWindow(surface, w, h);
	sdl.Show();

	// ── ImGui ─────────────────────────────────────────────────────────────────
	ImGui_ImplVulkan_InitInfo init_info = vulkan.MakeInitInfo();
	VulkanEmojiAtlas emoji_atlas(vulkan);
	ImGuiLayer imgui;
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
		emoji_atlas.DumpAtlasToPng("/tmp/emoji_atlas_vulkan.png");
		imgui.SetEmojiAtlas(&emoji_atlas);
	} else {
		std::println(stderr, "[main] Vulkan emoji atlas build failed");
	}

	TestEngineLayer test_engine;
	test_engine.Init();

	// ── Main loop ─────────────────────────────────────────────────────────────
	bool done = false;
	while (!done) {
		sdl.PollEvents(done, [&](const SDL_Event* event) { imgui.ProcessEvent(event); });
		done = done || imgui.RequestQuit;
		if (sdl.IsMinimized()) {
			SDL_Delay(10);
			continue;
		}

		sdl.GetSize(w, h);
		vulkan.RebuildSwapchainIfNeeded(w, h);

		imgui.NewFrame();
		imgui.BuildUI();
		test_engine.BuildUI(&imgui.ShowTestEngineWindow);
		imgui.Render();

		ImDrawData* draw_data = ImGui::GetDrawData();
		if (draw_data->DisplaySize.x > 0.0f && draw_data->DisplaySize.y > 0.0f) {
			vulkan.SetClearColor(imgui.ClearColor);
			vulkan.FrameRender(draw_data);
			vulkan.FramePresent();
			test_engine.PostSwap();
		}
	}

	// ── Cleanup ───────────────────────────────────────────────────────────────
	vulkan.WaitIdle();
	test_engine.Stop();
	imgui.SetEmojiAtlas(nullptr);
	emoji_atlas.Cleanup();
	imgui.Shutdown();
	test_engine.Shutdown();
	vulkan.CleanupWindow();
	vulkan.Cleanup();
	sdl.Shutdown();

	return 0;
}
