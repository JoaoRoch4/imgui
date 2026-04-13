#pragma once

#include "imgui_te_engine.h"

// TestEngineLayer wraps ImGuiTestEngine lifecycle, the test-runner window,
// and test registration.
//
// Lifetime:
//   Init()     — after ImGuiLayer::Init()  (ImGui context already exists)
//   BuildUI()  — inside the frame, between ImGuiLayer::NewFrame() and Render()
//   PostSwap() — after VulkanContext::FramePresent()
//   Stop()     — before ImGuiLayer::Shutdown()
//   Shutdown() — after  ImGuiLayer::Shutdown() (after ImGui::DestroyContext())
class TestEngineLayer {
    public:
	TestEngineLayer();

	ImGuiTestEngine* Engine;
	bool ShowWindow;

	// Create the engine, bind it to the current ImGui context, register tests.
	void Init();

	// Stop the engine (call BEFORE ImGuiLayer::Shutdown()).
	void Stop();

	// Destroy the engine context (call AFTER ImGuiLayer::Shutdown() / ImGui::DestroyContext()).
	void Shutdown();

	// Must be called once per frame after the framebuffer swap.
	void PostSwap();

	// Show the test-runner window.
	void BuildUI();

    private:
	void RegisterTests();
};
