#pragma once

#include <NoGraphicsAPIUtility/bump_allocator.hpp>

#include <imgui.h>

// Dear ImGui renderer backend over NoGraphicsAPI, Vulkan only. It sets RendererHasTextures and RendererHasVtxOffset.
// ImTextureID is the slot of the texture in the backend's texture descriptor heap. Slot 0 stays empty, because 0 is ImTextureID_Invalid.
// Build an ImGui frame (ImGui::NewFrame to RenderDrawData) only for a frame that you submit, with at most framesInFlight frames
// in flight: ImGui counts each built frame in UnusedFrames, and a texture that ImGui wants destroyed is destroyed when its
// UnusedFrames reaches framesInFlight.
struct ImGui_ImplNoGraphicsAPI_InitInfo
{
    gpu::Device* device = nullptr;
    gpu::Format colorFormat = gpu::Format::undefined; // The target format. An _srgb format linearizes the vertex colors.
    uint32 maxTextures = 64; // The font atlas, the user textures, and one more atlas while ImGui grows the atlas.
    uint32 framesInFlight = 2;
};

// Returns false when the PSO or a descriptor heap cannot be created. The error callback has the detail.
IMGUI_IMPL_API bool ImGui_ImplNoGraphicsAPI_Init(const ImGui_ImplNoGraphicsAPI_InitInfo* info);
// Call after wait_idle. Destroys every texture of ImGui::GetPlatformIO().Textures.
IMGUI_IMPL_API void ImGui_ImplNoGraphicsAPI_Shutdown();
IMGUI_IMPL_API void ImGui_ImplNoGraphicsAPI_NewFrame();

// Registers a texture that you create and own, for ImGui::Image(ImTextureRef(id), ...), and returns its slot. Create it with
// TextureUsage::sampled, in a color format that samples as float (not _uint, depth or stencil). Make it readable by the fragment
// stage before RenderDrawData. Its slot counts toward maxTextures.
IMGUI_IMPL_API ImTextureID ImGui_ImplNoGraphicsAPI_AddTexture(gpu::Texture* texture);
// Frees the slot of an AddTexture id. Call it only after no frame in flight uses the id. Shutdown frees every such slot and
// destroys none of the textures: destroy each texture after RemoveTexture or Shutdown.
IMGUI_IMPL_API void ImGui_ImplNoGraphicsAPI_RemoveTexture(ImTextureID id);
// Record outside a render pass, before RenderDrawData. Creates, updates and destroys the textures of data->Textures.
// The staging bytes come from bump: a new texture costs width * height * 4 bytes. A texture update costs UpdateRect.w * UpdateRect.h * 4
// bytes when the sum of the areas of its rects is at least half the area of UpdateRect, and w * h * 4 bytes for each rect otherwise. Each allocation rounds up to 16 bytes.
IMGUI_IMPL_API void ImGui_ImplNoGraphicsAPI_UpdateTextures(ImDrawData* data, gpu::CommandBuffer* commands, gpu::BumpAllocator& bump);

// Record inside a render pass on a color target of InitInfo::colorFormat, after UpdateTextures. One draw_indexed for each ImDrawCmd.
// The bytes come from bump: TotalVtxCount * 20 bytes of vertices, TotalIdxCount * sizeof(ImDrawIdx) bytes of indices, and 40 bytes
// for each root, each allocation rounded up to 16 bytes. A new root starts when the texture, the vertex pointer or the sampler changes.
// The draws sample linear until a DrawCallback_SetSamplerNearest of the platform IO, and again after DrawCallback_SetSamplerLinear or
// DrawCallback_ResetRenderState.
// It sets the viewport to the framebuffer size, and leaves that viewport, the backend PSO, the backend texture and sampler
// descriptor heaps and the last scissor bound.
IMGUI_IMPL_API void ImGui_ImplNoGraphicsAPI_RenderDrawData(ImDrawData* data, gpu::CommandBuffer* commands, gpu::BumpAllocator& bump);
