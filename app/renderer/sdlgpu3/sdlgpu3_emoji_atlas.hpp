#pragma once

#include "app/ui/emoji_atlas.hpp"
#include "app/renderer/sdlgpu3/sdlgpu3_context.hpp"
#include <SDL3/SDL_gpu.h>

// SDLGPU3EmojiAtlas uploads the EmojiAtlas pixel data as a SDL_GPUTexture and
// returns it as ImTextureID (SDL_GPUTexture* cast to uintptr_t).  The ImGui
// SDLGPU3 backend uses its own internal SamplerLinear when rendering a texture
// ID, so no separate sampler is needed.
//
// Lifetime: must outlive any frames that reference it.  Call Cleanup() (or let
// the destructor run) before destroying the SDLGPU3Context.
class SDLGPU3EmojiAtlas : public EmojiAtlas
{
public:
    explicit SDLGPU3EmojiAtlas(SDLGPU3Context& ctx);
    ~SDLGPU3EmojiAtlas() override;

    // Release the SDL_GPUTexture created during UploadRGBA().
    // Safe to call more than once.
    void Cleanup();

protected:
    ImTextureID UploadRGBA(const std::vector<uint8_t>& pixels,
                           int width, int height) override;

private:
    SDLGPU3Context& Ctx_;
    SDL_GPUTexture* Texture_ { nullptr };
};
