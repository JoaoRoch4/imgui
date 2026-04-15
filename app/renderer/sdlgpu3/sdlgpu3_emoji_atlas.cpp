#include "sdlgpu3_emoji_atlas.hpp"

#include <bit>
#include <cstring>
#include <print>

// ── SDLGPU3EmojiAtlas ─────────────────────────────────────────────────────────

SDLGPU3EmojiAtlas::SDLGPU3EmojiAtlas(SDLGPU3Context& ctx)
    : Ctx_ { ctx }
{
}

SDLGPU3EmojiAtlas::~SDLGPU3EmojiAtlas()
{
	Cleanup();
}

void SDLGPU3EmojiAtlas::Cleanup()
{
	if (Texture_ != nullptr) {
		SDL_ReleaseGPUTexture(Ctx_.Device, Texture_);
		Texture_ = nullptr;
		TextureID_ = ImTextureID_Invalid;
	}
}

ImTextureID SDLGPU3EmojiAtlas::UploadRGBA(const std::vector<uint8_t>& pixels,
	int width, int height)
{
	SDL_GPUDevice* dev = Ctx_.Device;

	// ── Create device-local texture ───────────────────────────────────────────
	{
		SDL_GPUTextureCreateInfo ci {};
		ci.type = SDL_GPU_TEXTURETYPE_2D;
		ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
		ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		ci.width = static_cast<Uint32>(width);
		ci.height = static_cast<Uint32>(height);
		ci.layer_count_or_depth = 1;
		ci.num_levels = 1;
		ci.sample_count = SDL_GPU_SAMPLECOUNT_1;

		Texture_ = SDL_CreateGPUTexture(dev, &ci);
		if (Texture_ == nullptr) {
			std::println(stderr, "[SDLGPU3EmojiAtlas] SDL_CreateGPUTexture failed: {}",
				SDL_GetError());
			return ImTextureID_Invalid;
		}
	}

	// ── Upload via transfer buffer ────────────────────────────────────────────
	const uint32_t upload_size = static_cast<uint32_t>(width * height * 4);
	const uint32_t upload_pitch = static_cast<uint32_t>(width * 4);

	SDL_GPUTransferBuffer* transfer_buf { nullptr };
	{
		SDL_GPUTransferBufferCreateInfo ci {};
		ci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
		ci.size = upload_size;

		transfer_buf = SDL_CreateGPUTransferBuffer(dev, &ci);
		if (transfer_buf == nullptr) {
			std::println(stderr,
				"[SDLGPU3EmojiAtlas] SDL_CreateGPUTransferBuffer failed: {}",
				SDL_GetError());
			SDL_ReleaseGPUTexture(dev, Texture_);
			Texture_ = nullptr;
			return ImTextureID_Invalid;
		}
	}

	// Copy pixel data into the transfer buffer.
	{
		void* mapped = SDL_MapGPUTransferBuffer(dev, transfer_buf, false);
		for (int y = 0; y < height; ++y) {
			std::memcpy(
				static_cast<uint8_t*>(mapped) + static_cast<ptrdiff_t>(y) * static_cast<ptrdiff_t>(upload_pitch),
				pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(upload_pitch),
				upload_pitch);
		}
		SDL_UnmapGPUTransferBuffer(dev, transfer_buf);
	}

	// Submit copy command.
	{
		SDL_GPUTextureTransferInfo transfer_info {};
		transfer_info.transfer_buffer = transfer_buf;
		transfer_info.offset = 0;
		transfer_info.pixels_per_row = static_cast<Uint32>(width);
		transfer_info.rows_per_layer = static_cast<Uint32>(height);

		SDL_GPUTextureRegion texture_region {};
		texture_region.texture = Texture_;
		texture_region.w = static_cast<Uint32>(width);
		texture_region.h = static_cast<Uint32>(height);
		texture_region.d = 1;

		SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
		SDL_GPUCopyPass* pass = SDL_BeginGPUCopyPass(cmd);
		SDL_UploadToGPUTexture(pass, &transfer_info, &texture_region, false);
		SDL_EndGPUCopyPass(pass);
		SDL_SubmitGPUCommandBuffer(cmd);
	}

	SDL_ReleaseGPUTransferBuffer(dev, transfer_buf);

	// ImTextureID for SDLGPU3 backend = SDL_GPUTexture* cast to uintptr_t.
	return static_cast<ImTextureID>(std::bit_cast<uintptr_t>(Texture_));
}
