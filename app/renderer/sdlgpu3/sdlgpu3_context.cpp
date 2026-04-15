#include "sdlgpu3_context.hpp"

#include <print>

SDLGPU3Context::SDLGPU3Context()
    : Device { nullptr }
    , Window { nullptr }
    , ReadbackTexture { nullptr }
    , ReadbackTransferBuf { nullptr }
    , ReadbackFormat { SDL_GPU_TEXTUREFORMAT_INVALID }
    , ReadbackW { 0 }
    , ReadbackH { 0 }
{
}

bool SDLGPU3Context::Setup(SDL_Window* window)
{
	Window = window;

	Device = SDL_CreateGPUDevice(
		SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB,
		true,
		"vulkan");
	if (Device == nullptr) {
		std::println("Error: SDL_CreateGPUDevice(): {}", SDL_GetError());
		return false;
	}

	if (!SDL_ClaimWindowForGPUDevice(Device, Window)) {
		std::println("Error: SDL_ClaimWindowForGPUDevice(): {}", SDL_GetError());
		SDL_DestroyGPUDevice(Device);
		Device = nullptr;
		Window = nullptr;
		return false;
	}

	SDL_SetGPUSwapchainParameters(
		Device, Window,
		SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
		SDL_GPU_PRESENTMODE_VSYNC);

	return true;
}

void SDLGPU3Context::Cleanup()
{
	if (Device && ReadbackTransferBuf) {
		SDL_ReleaseGPUTransferBuffer(Device, ReadbackTransferBuf);
		ReadbackTransferBuf = nullptr;
	}
	if (Device && ReadbackTexture) {
		SDL_ReleaseGPUTexture(Device, ReadbackTexture);
		ReadbackTexture = nullptr;
	}
	ReadbackW = 0;
	ReadbackH = 0;

	if (Device && Window)
		SDL_ReleaseWindowFromGPUDevice(Device, Window);

	if (Device)
		SDL_DestroyGPUDevice(Device);

	Device = nullptr;
	Window = nullptr;
}

void SDLGPU3Context::WaitIdle() const
{
	if (Device)
		SDL_WaitForGPUIdle(Device);
}

ImGui_ImplSDLGPU3_InitInfo SDLGPU3Context::MakeInitInfo() const
{
	ImGui_ImplSDLGPU3_InitInfo init_info = {};
	init_info.Device = Device;
	init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(Device, Window);
	init_info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
	init_info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
	init_info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
	return init_info;
}

void SDLGPU3Context::FrameRender(ImDrawData* draw_data, const ImVec4& clear_color)
{
	if (!Device || !Window)
		return;

	if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
		return;

	int win_w = 0;
	int win_h = 0;
	SDL_GetWindowSizeInPixels(Window, &win_w, &win_h);
	UpdateReadbackResources(win_w, win_h);

	SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(Device);
	if (!command_buffer)
		return;

	SDL_GPUTexture* swapchain_texture = nullptr;
	SDL_WaitAndAcquireGPUSwapchainTexture(command_buffer, Window, &swapchain_texture, nullptr, nullptr);

	if (swapchain_texture != nullptr && ReadbackTexture != nullptr) {
		ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, command_buffer);

		// 1. Render ImGui into the readback texture (owned, downloadable).
		SDL_GPUColorTargetInfo target_info = {};
		target_info.texture = ReadbackTexture;
		target_info.clear_color = SDL_FColor { clear_color.x, clear_color.y, clear_color.z, clear_color.w };
		target_info.load_op = SDL_GPU_LOADOP_CLEAR;
		target_info.store_op = SDL_GPU_STOREOP_STORE;
		target_info.mip_level = 0;
		target_info.layer_or_depth_plane = 0;
		target_info.cycle = false;

		SDL_GPURenderPass* render_pass = SDL_BeginGPURenderPass(command_buffer, &target_info, 1, nullptr);
		ImGui_ImplSDLGPU3_RenderDrawData(draw_data, command_buffer, render_pass);
		SDL_EndGPURenderPass(render_pass);

		// 2. Blit readback texture → swapchain for presentation.
		SDL_GPUBlitInfo blit = {};
		blit.source.texture = ReadbackTexture;
		blit.source.w = static_cast<Uint32>(win_w);
		blit.source.h = static_cast<Uint32>(win_h);
		blit.destination.texture = swapchain_texture;
		blit.destination.w = static_cast<Uint32>(win_w);
		blit.destination.h = static_cast<Uint32>(win_h);
		blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
		blit.filter = SDL_GPU_FILTER_NEAREST;
		blit.cycle = false;
		SDL_BlitGPUTexture(command_buffer, &blit);

		// 3. Download readback texture → transfer buffer for ReadPixels().
		SDL_GPUTextureRegion src_region = {};
		src_region.texture = ReadbackTexture;
		src_region.w = static_cast<Uint32>(win_w);
		src_region.h = static_cast<Uint32>(win_h);
		src_region.d = 1;

		SDL_GPUTextureTransferInfo dst_info = {};
		dst_info.transfer_buffer = ReadbackTransferBuf;
		dst_info.pixels_per_row = static_cast<Uint32>(win_w);
		dst_info.rows_per_layer = static_cast<Uint32>(win_h);

		SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);
		SDL_DownloadFromGPUTexture(copy_pass, &src_region, &dst_info);
		SDL_EndGPUCopyPass(copy_pass);
	}

	SDL_SubmitGPUCommandBuffer(command_buffer);
}

void SDLGPU3Context::UpdateReadbackResources(int w, int h)
{
	if (ReadbackW == w && ReadbackH == h)
		return;

	if (ReadbackTransferBuf) {
		SDL_ReleaseGPUTransferBuffer(Device, ReadbackTransferBuf);
		ReadbackTransferBuf = nullptr;
	}
	if (ReadbackTexture) {
		SDL_ReleaseGPUTexture(Device, ReadbackTexture);
		ReadbackTexture = nullptr;
	}
	ReadbackW = 0;
	ReadbackH = 0;

	if (w <= 0 || h <= 0)
		return;

	ReadbackFormat = SDL_GetGPUSwapchainTextureFormat(Device, Window);

	SDL_GPUTextureCreateInfo tex_info = {};
	tex_info.type = SDL_GPU_TEXTURETYPE_2D;
	tex_info.format = ReadbackFormat;
	tex_info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
	tex_info.width = static_cast<Uint32>(w);
	tex_info.height = static_cast<Uint32>(h);
	tex_info.layer_count_or_depth = 1;
	tex_info.num_levels = 1;
	tex_info.sample_count = SDL_GPU_SAMPLECOUNT_1;

	ReadbackTexture = SDL_CreateGPUTexture(Device, &tex_info);
	if (!ReadbackTexture) {
		std::println(stderr, "[sdlgpu3] SDL_CreateGPUTexture for readback failed: {}", SDL_GetError());
		return;
	}

	SDL_GPUTransferBufferCreateInfo buf_info = {};
	buf_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
	buf_info.size = static_cast<Uint32>(w * h * 4);

	ReadbackTransferBuf = SDL_CreateGPUTransferBuffer(Device, &buf_info);
	if (!ReadbackTransferBuf) {
		std::println(stderr, "[sdlgpu3] SDL_CreateGPUTransferBuffer for readback failed: {}", SDL_GetError());
		SDL_ReleaseGPUTexture(Device, ReadbackTexture);
		ReadbackTexture = nullptr;
		return;
	}

	ReadbackW = w;
	ReadbackH = h;
}

bool SDLGPU3Context::ReadPixels(int x, int y, int w, int h, unsigned int* pixels)
{
	if (!ReadbackTexture || !ReadbackTransferBuf || w <= 0 || h <= 0)
		return false;

	SDL_WaitForGPUIdle(Device);

	const void* mapped = SDL_MapGPUTransferBuffer(Device, ReadbackTransferBuf, false);
	if (!mapped)
		return false;

	const bool is_bgra = (ReadbackFormat == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
	                      ReadbackFormat == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB);

	const auto* src = static_cast<const unsigned char*>(mapped);
	for (int row = 0; row < h; ++row) {
		const unsigned char* row_src = src + (static_cast<ptrdiff_t>(y + row) * ReadbackW + x) * 4;
		unsigned int* row_dst = pixels + row * w;
		for (int col = 0; col < w; ++col) {
			const unsigned char r_byte = row_src[col * 4 + (is_bgra ? 2 : 0)];
			const unsigned char g_byte = row_src[col * 4 + 1];
			const unsigned char b_byte = row_src[col * 4 + (is_bgra ? 0 : 2)];
			const unsigned char a_byte = row_src[col * 4 + 3];
			row_dst[col] = static_cast<unsigned int>(r_byte) |
			               (static_cast<unsigned int>(g_byte) << 8) |
			               (static_cast<unsigned int>(b_byte) << 16) |
			               (static_cast<unsigned int>(a_byte) << 24);
		}
	}

	SDL_UnmapGPUTransferBuffer(Device, ReadbackTransferBuf);
	return true;
}
