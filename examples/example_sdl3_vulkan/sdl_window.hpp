#pragma once

#include "imgui.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vector>

class ImGuiLayer; // forward declaration — avoids circular include

// SDLWindow owns the OS window, SDL lifecycle, and Vulkan surface creation.
class SDLWindow
{
public:
    SDL_Window* Window    = nullptr;
    float       MainScale = 1.0f;

    // Initialise SDL, create the window, compute DPI scale.  Returns false on error.
    bool Init(const char* title, int width, int height);

    // Destroy the window and shut down SDL.
    void Shutdown();

    // Collect the Vulkan instance extensions required by SDL.
    std::vector<const char*> GetVulkanExtensions() const;

    // Create a Vulkan surface for this window.  Returns VK_NULL_HANDLE on failure.
    VkSurfaceKHR CreateVulkanSurface(VkInstance instance,
                                     VkAllocationCallbacks* allocator) const;

    // Query the current window size in pixels.
    void GetSize(int& w, int& h) const;

    // Make the window visible.
    void Show();

    // Returns true if the window is currently minimised.
    bool IsMinimized() const;

    // Drain the SDL event queue.  Sets done=true on quit/close events.
    // Forwards every event to imgui->ProcessEvent() when imgui != nullptr.
    void PollEvents(bool& done, ImGuiLayer* imgui);
};
