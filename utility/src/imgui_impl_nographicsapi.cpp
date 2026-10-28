#include <NoGraphicsAPIUtility/imgui_impl_nographicsapi.h>
#include "imgui_impl_nographicsapi_shared.h"
#include "imgui_impl_nographicsapi_spirv.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

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

// Packs the rect as RGBA8 into bump storage and records its copy. Alpha8 texels become white with that alpha.
void upload_rect(gpu::CommandBuffer* commands, gpu::BumpAllocator& bump, gpu::Texture* destination, ImTextureData* texture, ImTextureRect rect) noexcept
{
    const uint64 row_bytes = uint64(rect.w) * 4;
    const gpu::GpuCpuRange<byte> staging = bump.allocate(row_bytes * rect.h);
    assert(staging.size && "the bump allocator has no room for the texture bytes");
    for (uint32 y = 0; y < rect.h; ++y)
    {
        const byte* source = static_cast<const byte*>(texture->GetPixelsAt(rect.x, rect.y + int(y)));
        byte* row = staging.cpu + y * row_bytes;
        if (texture->Format == ImTextureFormat_RGBA32)
        {
            memcpy(row, source, size_t(row_bytes));
        }
        else
        {
            for (uint32 x = 0; x < rect.w; ++x)
            {
                row[x * 4 + 0] = 255;
                row[x * 4 + 1] = 255;
                row[x * 4 + 2] = 255;
                row[x * 4 + 3] = source[x];
            }
        }
    }
    gpu::copy_memory_to_texture(commands, gpu::gpu_range(staging), destination, {
        .offset = {.x = rect.x, .y = rect.y, .z = 0},
        .extent = {.x = rect.w, .y = rect.h, .z = 1},
    });
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

void ImGui_ImplNoGraphicsAPI_UpdateTextures(ImDrawData* draw_data, gpu::CommandBuffer* commands, gpu::BumpAllocator& bump)
{
    BackendData* data = static_cast<BackendData*>(ImGui::GetIO().BackendRendererUserData);
    assert(data && draw_data && commands && "call ImGui_ImplNoGraphicsAPI_Init first");
    if (!draw_data->Textures) return;
    bool copied = false;
    for (ImTextureData* texture : *draw_data->Textures)
    {
        if (texture->Status == ImTextureStatus_WantCreate)
        {
            assert(texture->Width > 0 && texture->Height > 0 && data->free_count && "maxTextures is exhausted");
            const uint32 slot = data->free_slots[--data->free_count];
            const gpu::TextureDesc desc{
                .extent = {.x = uint32(texture->Width), .y = uint32(texture->Height), .z = 1},
                .format = gpu::Format::rgba8_unorm,
                .usage = gpu::TextureUsage::sampled | gpu::TextureUsage::transfer_destination,
            };
            TextureSlot& entry = data->slots[slot];
            entry.heap = gpu::create_texture_heap(data->device, gpu::get_texture_size_align(data->device, desc).size);
            entry.texture = gpu::create_texture(data->device, desc, entry.heap, 0);
            assert(entry.texture && "texture creation failed");
            gpu::initialize_texture(commands, entry.texture);
            gpu::write_texture_descriptor(data->textures, slot, entry.texture, gpu::TextureDescriptorType::sampled);
            upload_rect(commands, bump, entry.texture, texture,
                        {.x = 0, .y = 0, .w = uint16(texture->Width), .h = uint16(texture->Height)});
            texture->SetTexID(ImTextureID(slot));
            texture->SetStatus(ImTextureStatus_OK);
            copied = true;
        }
        else if (texture->Status == ImTextureStatus_WantUpdates)
        {
            gpu::Texture* destination = data->slots[uint32(texture->TexID)].texture;
            for (const ImTextureRect& rect : texture->Updates) upload_rect(commands, bump, destination, texture, rect);
            texture->SetStatus(ImTextureStatus_OK);
            copied = true;
        }
        else if (texture->Status == ImTextureStatus_WantDestroy && texture->UnusedFrames >= int(data->frames_in_flight))
        {
            release_texture(*data, texture);
        }
    }
    if (copied) gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::fragment, gpu::Access::shader_read);
}
