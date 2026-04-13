#include "sdl_window.hpp"
#include "imgui_layer.hpp"   // needed for ImGuiLayer::ProcessEvent in PollEvents
#include <cstdio>            // printf

bool SDLWindow::Init(const char* title, int width, int height)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        std::printf("Error: SDL_Init(): %s\n", SDL_GetError());
        return false;
    }

    MainScale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());

    SDL_WindowFlags window_flags =
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;

    Window = SDL_CreateWindow(title,
                              static_cast<int>(width  * MainScale),
                              static_cast<int>(height * MainScale),
                              window_flags);
    if (Window == nullptr)
    {
        std::printf("Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        return false;
    }
    return true;
}

void SDLWindow::Shutdown()
{
    SDL_DestroyWindow(Window);
    Window = nullptr;
    SDL_Quit();
}

ImVector<const char*> SDLWindow::GetVulkanExtensions() const
{
    ImVector<const char*> extensions;
    uint32_t count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    for (uint32_t n = 0; n < count; n++)
        extensions.push_back(sdl_extensions[n]);
    return extensions;
}

VkSurfaceKHR SDLWindow::CreateVulkanSurface(VkInstance instance,
                                             VkAllocationCallbacks* allocator) const
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (SDL_Vulkan_CreateSurface(Window, instance, allocator, &surface) == 0)
    {
        std::printf("Failed to create Vulkan surface.\n");
        return VK_NULL_HANDLE;
    }
    return surface;
}

void SDLWindow::GetSize(int& w, int& h) const
{
    SDL_GetWindowSize(Window, &w, &h);
}

void SDLWindow::Show()
{
    SDL_SetWindowPosition(Window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(Window);
}

bool SDLWindow::IsMinimized() const
{
    return (SDL_GetWindowFlags(Window) & SDL_WINDOW_MINIMIZED) != 0;
}

void SDLWindow::PollEvents(bool& done, ImGuiLayer* imgui)
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (imgui)
            imgui->ProcessEvent(&event);
        if (event.type == SDL_EVENT_QUIT)
            done = true;
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
            event.window.windowID == SDL_GetWindowID(Window))
            done = true;
    }
}
