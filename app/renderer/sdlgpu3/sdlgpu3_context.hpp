#pragma once

#include "imgui.h"
#include "imgui_impl_sdlgpu3.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

// SDLGPU3Context owns SDL GPU device lifecycle and per-frame render submission.
class SDLGPU3Context
{
public:
    SDLGPU3Context();

    SDL_GPUDevice*         Device;
    SDL_Window*            Window;
    SDL_GPUTexture*        ReadbackTexture;
    SDL_GPUTransferBuffer* ReadbackTransferBuf;
    SDL_GPUTextureFormat   ReadbackFormat;
    int                    ReadbackW;
    int                    ReadbackH;

    // Create SDL GPU device, claim the window, and configure swapchain params.
    bool Setup(SDL_Window* window);

    // Release resources created in Setup().
    void Cleanup();

    // Block until GPU finishes work.
    void WaitIdle() const;

    // Build init info for ImGui SDLGPU3 backend.
    ImGui_ImplSDLGPU3_InitInfo MakeInitInfo() const;

    // Render ImGui draw data. Renders into ReadbackTexture, blits to swapchain,
    // and downloads pixels to ReadbackTransferBuf for ScreenCaptureFunc.
    void FrameRender(ImDrawData* draw_data, const ImVec4& clear_color);

    // Copy the (x, y, w, h) region from the last rendered frame into pixels[].
    // pixels must point to at least w * h unsigned int values (RGBA byte order).
    bool ReadPixels(int x, int y, int w, int h, unsigned int* pixels);

private:
    // (Re-)create ReadbackTexture and ReadbackTransferBuf when size changes.
    void UpdateReadbackResources(int w, int h);
};
