#pragma once

#include <NoGraphicsAPIUtility/bump_allocator.hpp>

#include <imgui.h>

// Dear ImGui renderer backend over NoGraphicsAPI, Vulkan only. It sets RendererHasTextures and RendererHasVtxOffset.
// ImTextureID is the slot of the texture in the backend's texture descriptor heap. Slot 0 stays empty, because 0 is ImTextureID_Invalid.
// Call NewFrame once for each submitted frame, with at most framesInFlight frames in flight: a texture that ImGui
// wants destroyed is destroyed when its UnusedFrames reaches framesInFlight.
struct ImGui_ImplNoGraphicsAPI_InitInfo
{
    gpu::Device* device = nullptr;
    gpu::Format colorFormat = gpu::Format::undefined; // The target format. An _srgb format linearizes the vertex colors.
    uint32 maxTextures = 64; // The font atlas and the user textures.
    uint32 framesInFlight = 2;
};

// Returns false when the PSO or a descriptor heap cannot be created. The error callback has the detail.
IMGUI_IMPL_API bool ImGui_ImplNoGraphicsAPI_Init(const ImGui_ImplNoGraphicsAPI_InitInfo* info);
// Call after wait_idle. Destroys every texture of ImGui::GetPlatformIO().Textures.
IMGUI_IMPL_API void ImGui_ImplNoGraphicsAPI_Shutdown();
IMGUI_IMPL_API void ImGui_ImplNoGraphicsAPI_NewFrame();
// Record outside a render pass, before RenderDrawData. Creates, updates and destroys the textures of data->Textures.
// The staging bytes come from bump: a new texture costs width * height * 4 bytes, and each rect of a texture update costs
// w * h * 4 bytes. Each of these allocations rounds up to 16 bytes.
IMGUI_IMPL_API void ImGui_ImplNoGraphicsAPI_UpdateTextures(ImDrawData* data, gpu::CommandBuffer* commands, gpu::BumpAllocator& bump);

// Record inside a render pass on a color target of InitInfo::colorFormat, after UpdateTextures. One draw_indexed for each ImDrawCmd.
// The bytes come from bump: TotalVtxCount * 20 bytes of vertices, TotalIdxCount * sizeof(ImDrawIdx) bytes of indices, and 32 bytes
// for each root, each allocation rounded up to 16 bytes. A new root starts when the texture or the vertex offset changes.
// It leaves the backend PSO, the backend texture and sampler descriptor heaps and the last scissor bound.
IMGUI_IMPL_API void ImGui_ImplNoGraphicsAPI_RenderDrawData(ImDrawData* data, gpu::CommandBuffer* commands, gpu::BumpAllocator& bump);
