#pragma once

#include "imgui_te_engine.h"

// TestEngineLayer wraps ImGuiTestEngine lifecycle, the test-runner window,
// and test registration.
//
// Lifetime:
//   Init()     — after ImGuiLayer::Init()  (ImGui context already exists)
//   BuildUI()  — inside the frame, between ImGuiLayer::NewFrame() and Render()
//   PostSwap() — after VulkanContext::FramePresent()
//   Shutdown() — before ImGuiLayer::Shutdown()
class TestEngineLayer
{
public:
    ImGuiTestEngine* Engine     = nullptr;
    bool             ShowWindow = true;

    // Create the engine, bind it to the current ImGui context, register tests.
    void Init();

    // Stop and destroy the engine.
    void Shutdown();

    // Must be called once per frame after the framebuffer swap.
    void PostSwap();

    // Show the test-runner window.
    void BuildUI();

private:
    void RegisterTests();
};
