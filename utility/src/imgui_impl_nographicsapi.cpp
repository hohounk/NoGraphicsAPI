#include <NoGraphicsAPIUtility/imgui_impl_nographicsapi.h>
#include "imgui_impl_nographicsapi_shared.h"
#include "imgui_impl_nographicsapi_spirv.h"

#include <assert.h>
#include <stddef.h>

#if defined(IMGUI_USE_BGRA_PACKED_COLOR)
#error "The shader reads ImDrawVert::col as RGBA8: IMGUI_USE_BGRA_PACKED_COLOR is not supported"
#endif
static_assert(sizeof(ImDrawVert) == sizeof(ImGuiNgaVertex) && offsetof(ImDrawVert, pos) == offsetof(ImGuiNgaVertex, position) &&
              offsetof(ImDrawVert, uv) == offsetof(ImGuiNgaVertex, uv) && offsetof(ImDrawVert, col) == offsetof(ImGuiNgaVertex, color));

namespace
{

struct TextureSlot
{
    gpu::Texture* texture = nullptr;
    gpu::TextureHeap heap{};
};

struct BackendData
{
    gpu::Device* device = nullptr;
    gpu::PSO* pso = nullptr;
    gpu::TextureDescriptorHeap* textures = nullptr;
    gpu::SamplerDescriptorHeap* samplers = nullptr;
    TextureSlot* slots = nullptr; // maxTextures + 1 entries. Slot 0 stays empty.
    uint32* free_slots = nullptr; // A stack of free slots; the lowest slot is on top.
    uint32 free_count = 0;
    uint32 frames_in_flight = 0;
    uint32 srgb = 0;
};

void release_texture(BackendData& data, ImTextureData* texture) noexcept
{
    const uint32 slot = uint32(texture->TexID);
    gpu::destroy_texture(data.slots[slot].texture);
    gpu::destroy_texture_heap(data.slots[slot].heap);
    data.slots[slot] = {};
    data.free_slots[data.free_count++] = slot;
    texture->SetTexID(ImTextureID_Invalid);
    texture->SetStatus(ImTextureStatus_Destroyed);
}

} // namespace

bool ImGui_ImplNoGraphicsAPI_Init(const ImGui_ImplNoGraphicsAPI_InitInfo* info)
{
    IMGUI_CHECKVERSION();
    ImGuiIO& io = ImGui::GetIO();
    assert(info && info->device && info->colorFormat != gpu::Format::undefined && info->maxTextures && info->framesInFlight);
    assert(!io.BackendRendererUserData && "a renderer backend is already initialized");

    gpu::PSO* pso = gpu::create_graphics_pso(info->device, {
        .vertex = {.code = {imgui_vertex_spirv, sizeof(imgui_vertex_spirv)}, .entry_point = "vertexMain"},
        .fragment = {.code = {imgui_fragment_spirv, sizeof(imgui_fragment_spirv)}, .entry_point = "fragmentMain"},
        .color_targets = {{
            .format = info->colorFormat,
            .blend = {
                .enabled = true,
                .color = {.source = gpu::BlendFactor::source_alpha, .destination = gpu::BlendFactor::one_minus_source_alpha},
                .alpha = {.source = gpu::BlendFactor::one, .destination = gpu::BlendFactor::one_minus_source_alpha},
            },
        }},
    });
    gpu::TextureDescriptorHeap* textures = gpu::create_texture_descriptor_heap(info->device, info->maxTextures + 1);
    gpu::SamplerDescriptorHeap* samplers = gpu::create_sampler_descriptor_heap(info->device, 1);
    if (!pso || !textures || !samplers)
    {
        gpu::destroy_sampler_descriptor_heap(samplers);
        gpu::destroy_texture_descriptor_heap(textures);
        if (pso) gpu::destroy_pso(pso);
        return false;
    }
    gpu::write_sampler_descriptor(samplers, 0, {
        .address_u = gpu::AddressMode::clamp_to_edge,
        .address_v = gpu::AddressMode::clamp_to_edge,
        .address_w = gpu::AddressMode::clamp_to_edge,
    });

    const gpu::Format format = info->colorFormat;
    BackendData* data = new BackendData{
        .device = info->device,
        .pso = pso,
        .textures = textures,
        .samplers = samplers,
        .slots = new TextureSlot[info->maxTextures + 1]{},
        .free_slots = new uint32[info->maxTextures],
        .free_count = info->maxTextures,
        .frames_in_flight = info->framesInFlight,
        .srgb = format == gpu::Format::r8_srgb || format == gpu::Format::rg8_srgb || format == gpu::Format::rgba8_srgb || format == gpu::Format::bgra8_srgb,
    };
    for (uint32 index = 0; index < info->maxTextures; ++index) data->free_slots[index] = info->maxTextures - index;

    io.BackendRendererUserData = data;
    io.BackendRendererName = "imgui_impl_nographicsapi";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    return true;
}

void ImGui_ImplNoGraphicsAPI_Shutdown()
{
    ImGuiIO& io = ImGui::GetIO();
    BackendData* data = static_cast<BackendData*>(io.BackendRendererUserData);
    assert(data && "no renderer backend to shut down");
    for (ImTextureData* texture : ImGui::GetPlatformIO().Textures)
        if (texture->RefCount == 1 && texture->TexID != ImTextureID_Invalid) release_texture(*data, texture);
    gpu::destroy_sampler_descriptor_heap(data->samplers);
    gpu::destroy_texture_descriptor_heap(data->textures);
    gpu::destroy_pso(data->pso);
    delete[] data->free_slots;
    delete[] data->slots;
    delete data;
    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
}

void ImGui_ImplNoGraphicsAPI_NewFrame()
{
    assert(ImGui::GetIO().BackendRendererUserData && "call ImGui_ImplNoGraphicsAPI_Init first");
}
