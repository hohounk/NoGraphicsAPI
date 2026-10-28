#include <NoGraphicsAPIUtility/imgui_impl_nographicsapi.h>
#include "imgui_impl_nographicsapi_shared.h"
#include "imgui_impl_nographicsapi_spirv.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#if defined(IMGUI_USE_BGRA_PACKED_COLOR)
#error "The shader reads ImDrawVert::col as RGBA8: IMGUI_USE_BGRA_PACKED_COLOR is not supported"
#endif
static_assert(sizeof(ImDrawVert) == sizeof(ImGuiNgaVertex) && offsetof(ImDrawVert, pos) == offsetof(ImGuiNgaVertex, position) &&
              offsetof(ImDrawVert, uv) == offsetof(ImGuiNgaVertex, uv) && offsetof(ImDrawVert, col) == offsetof(ImGuiNgaVertex, color));
static_assert(sizeof(ImGuiNgaVertex) == 20);
static_assert(sizeof(ImGuiNgaRoot) == 40);

namespace
{

struct TextureSlot
{
    gpu::Texture* texture = nullptr;
    gpu::TextureHeap heap{};
    bool external = false; // A texture of ImGui_ImplNoGraphicsAPI_AddTexture, which the caller owns.
};

struct BackendData
{
    gpu::Device* device = nullptr;
    gpu::PSO* pso = nullptr;
    gpu::TextureDescriptorHeap* textures = nullptr;
    gpu::SamplerDescriptorHeap* samplers = nullptr;
    TextureSlot* slots = nullptr; // maxTextures + 1 entries. Slot 0 stays empty; a slot without a texture is free.
    uint32 max_textures = 0;
    uint32 frames_in_flight = 0;
    uint32 srgb = 0;
    uint32 sampler = 0; // The sampler descriptor of the draws that follow: 0 linear, 1 nearest.
};

uint32 allocate_slot(const BackendData& data) noexcept
{
    uint32 slot = 1;
    while (slot <= data.max_textures && data.slots[slot].texture) ++slot;
    assert(slot <= data.max_textures && "maxTextures is exhausted");
    return slot;
}

void release_texture(BackendData& data, ImTextureData* texture) noexcept
{
    const uint32 slot = uint32(texture->TexID);
    assert(!data.slots[slot].external);
    gpu::destroy_texture(data.slots[slot].texture);
    gpu::destroy_texture_heap(data.slots[slot].heap);
    data.slots[slot] = {};
    texture->SetTexID(ImTextureID_Invalid);
    texture->SetStatus(ImTextureStatus_Destroyed);
}

// Packs the rect as RGBA8 into bump storage and records its copy. Alpha8 texels become white with that alpha.
void upload_rect(gpu::CommandBuffer* commands, gpu::BumpAllocator& bump, gpu::Texture* destination, ImTextureData* texture, ImTextureRect rect) noexcept
{
    const uint64 row_bytes = uint64(rect.w) * 4;
    const gpu::GpuCpuRange<byte> staging = bump.allocate(row_bytes * rect.h);
    assert(staging.size && "the bump allocator has no room for the texture bytes");
    if (texture->Format == ImTextureFormat_RGBA32 && rect.w == texture->Width)
    {
        memcpy(staging.cpu, texture->GetPixelsAt(rect.x, rect.y), size_t(row_bytes * rect.h));
    }
    else if (texture->Format == ImTextureFormat_RGBA32)
    {
        for (uint32 y = 0; y < rect.h; ++y) memcpy(staging.cpu + y * row_bytes, texture->GetPixelsAt(rect.x, rect.y + int(y)), size_t(row_bytes));
    }
    else
    {
        for (uint32 y = 0; y < rect.h; ++y)
        {
            const byte* source = static_cast<const byte*>(texture->GetPixelsAt(rect.x, rect.y + int(y)));
            uint32* row = reinterpret_cast<uint32*>(staging.cpu + y * row_bytes);
            for (uint32 x = 0; x < rect.w; ++x) row[x] = (uint32(source[x]) << 24) | 0x00ffffffu;
        }
    }
    gpu::copy_memory_to_texture(commands, gpu::gpu_range(staging), destination, {
        .offset = {.x = rect.x, .y = rect.y, .z = 0},
        .extent = {.x = rect.w, .y = rect.h, .z = 1},
    });
}

// The value of ImGui::GetPlatformIO().DrawCallback_ResetRenderState. RenderDrawData compares against it and never calls it.
void reset_render_state(const ImDrawList*, const ImDrawCmd*) noexcept
{
}

// The values of DrawCallback_SetSamplerLinear and DrawCallback_SetSamplerNearest. RenderDrawData calls them. Their bodies differ, so that
// identical code folding cannot give them the address of reset_render_state.
void set_sampler_linear(const ImDrawList*, const ImDrawCmd*) noexcept
{
    static_cast<BackendData*>(ImGui::GetIO().BackendRendererUserData)->sampler = 0;
}

void set_sampler_nearest(const ImDrawList*, const ImDrawCmd*) noexcept
{
    static_cast<BackendData*>(ImGui::GetIO().BackendRendererUserData)->sampler = 1;
}

void bind_render_state(const BackendData& data, gpu::CommandBuffer* commands, float width, float height) noexcept
{
    gpu::set_viewport(commands, {.width = width, .height = height});
    gpu::set_texture_descriptor_heap(commands, data.textures);
    gpu::set_sampler_descriptor_heap(commands, data.samplers);
    gpu::bind_pso(commands, data.pso);
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
    gpu::SamplerDescriptorHeap* samplers = gpu::create_sampler_descriptor_heap(info->device, 2);
    if (!pso || !textures || !samplers)
    {
        gpu::destroy_sampler_descriptor_heap(samplers);
        gpu::destroy_texture_descriptor_heap(textures);
        gpu::destroy_pso(pso);
        return false;
    }
    gpu::write_sampler_descriptor(samplers, 0, {
        .address_u = gpu::AddressMode::clamp_to_edge,
        .address_v = gpu::AddressMode::clamp_to_edge,
        .address_w = gpu::AddressMode::clamp_to_edge,
    });
    gpu::write_sampler_descriptor(samplers, 1, {
        .min_filter = gpu::Filter::nearest,
        .mag_filter = gpu::Filter::nearest,
        .mip_filter = gpu::Filter::nearest,
        .address_u = gpu::AddressMode::clamp_to_edge,
        .address_v = gpu::AddressMode::clamp_to_edge,
        .address_w = gpu::AddressMode::clamp_to_edge,
    });

    BackendData* data = new BackendData{
        .device = info->device,
        .pso = pso,
        .textures = textures,
        .samplers = samplers,
        .slots = new TextureSlot[info->maxTextures + 1]{},
        .max_textures = info->maxTextures,
        .frames_in_flight = info->framesInFlight,
        .srgb = gpu::get_texture_format_info(info->colorFormat).srgb,
    };

    io.BackendRendererUserData = data;
    io.BackendRendererName = "imgui_impl_nographicsapi";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.DrawCallback_ResetRenderState = reset_render_state;
    platform.DrawCallback_SetSamplerLinear = set_sampler_linear;
    platform.DrawCallback_SetSamplerNearest = set_sampler_nearest;
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
    delete[] data->slots;
    delete data;
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    platform.DrawCallback_ResetRenderState = nullptr;
    platform.DrawCallback_SetSamplerLinear = nullptr;
    platform.DrawCallback_SetSamplerNearest = nullptr;
    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
}

void ImGui_ImplNoGraphicsAPI_NewFrame()
{
    assert(ImGui::GetIO().BackendRendererUserData && "call ImGui_ImplNoGraphicsAPI_Init first");
}

ImTextureID ImGui_ImplNoGraphicsAPI_AddTexture(gpu::Texture* texture)
{
    BackendData* data = static_cast<BackendData*>(ImGui::GetIO().BackendRendererUserData);
    assert(data && texture && "call ImGui_ImplNoGraphicsAPI_Init first");
    const uint32 slot = allocate_slot(*data);
    data->slots[slot] = {.texture = texture, .external = true};
    gpu::write_texture_descriptor(data->textures, slot, texture, gpu::TextureDescriptorType::sampled);
    return ImTextureID(slot);
}

void ImGui_ImplNoGraphicsAPI_RemoveTexture(ImTextureID id)
{
    BackendData* data = static_cast<BackendData*>(ImGui::GetIO().BackendRendererUserData);
    assert(data && "call ImGui_ImplNoGraphicsAPI_Init first");
    const uint32 slot = uint32(id);
    assert(slot >= 1 && slot <= data->max_textures && data->slots[slot].external && "the id is not a texture of AddTexture");
    data->slots[slot] = {};
}

void ImGui_ImplNoGraphicsAPI_UpdateTextures(ImDrawData* draw_data, gpu::CommandBuffer* commands, gpu::BumpAllocator& bump)
{
    BackendData* data = static_cast<BackendData*>(ImGui::GetIO().BackendRendererUserData);
    assert(data && draw_data && commands && "call ImGui_ImplNoGraphicsAPI_Init first");
    if (!draw_data->Textures) return;
    bool copied = false;
    bool updated = false;
    for (ImTextureData* texture : *draw_data->Textures)
    {
        if (texture->Status == ImTextureStatus_WantCreate)
        {
            assert(texture->Width > 0 && texture->Height > 0);
            const uint32 slot = allocate_slot(*data);
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
            // Earlier frames in flight may still sample the texture, and two update rects may overlap.
            if (!updated) gpu::barrier(commands, gpu::Stage::fragment | gpu::Stage::transfer, gpu::Access::shader_read | gpu::Access::transfer_write,
                                       gpu::Stage::transfer, gpu::Access::transfer_write);
            updated = true;
            gpu::Texture* destination = data->slots[uint32(texture->TexID)].texture;
            // One upload of UpdateRect, the bounding box, replaces one per rect when the rects cover at least half of it.
            uint64 rect_texels = 0;
            for (const ImTextureRect& rect : texture->Updates) rect_texels += uint64(rect.w) * rect.h;
            if (rect_texels && uint64(texture->UpdateRect.w) * texture->UpdateRect.h <= 2 * rect_texels)
                upload_rect(commands, bump, destination, texture, texture->UpdateRect);
            else
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

void ImGui_ImplNoGraphicsAPI_RenderDrawData(ImDrawData* draw_data, gpu::CommandBuffer* commands, gpu::BumpAllocator& bump)
{
    BackendData* data = static_cast<BackendData*>(ImGui::GetIO().BackendRendererUserData);
    assert(data && draw_data && commands && "call ImGui_ImplNoGraphicsAPI_Init first");
    const float framebuffer_width = draw_data->DisplaySize.x * draw_data->FramebufferScale.x;
    const float framebuffer_height = draw_data->DisplaySize.y * draw_data->FramebufferScale.y;
    if (framebuffer_width <= 0.0f || framebuffer_height <= 0.0f || draw_data->CmdLists.Size == 0) return;

    gpu::GpuCpuRange<ImGuiNgaVertex> vertices{};
    gpu::GpuCpuRange<ImDrawIdx> indices{};
    if (draw_data->TotalVtxCount > 0)
    {
        vertices = bump.allocate<ImGuiNgaVertex>(uint64(draw_data->TotalVtxCount));
        indices = bump.allocate<ImDrawIdx>(uint64(draw_data->TotalIdxCount));
        assert(vertices.size && indices.size && "the bump allocator has no room for the vertices and indices");
        ImGuiNgaVertex* vertex_cursor = vertices.cpu;
        ImDrawIdx* index_cursor = indices.cpu;
        for (const ImDrawList* list : draw_data->CmdLists)
        {
            memcpy(vertex_cursor, list->VtxBuffer.Data, size_t(list->VtxBuffer.Size) * sizeof(ImDrawVert));
            memcpy(index_cursor, list->IdxBuffer.Data, size_t(list->IdxBuffer.Size) * sizeof(ImDrawIdx));
            vertex_cursor += list->VtxBuffer.Size;
            index_cursor += list->IdxBuffer.Size;
        }
    }

    constexpr gpu::IndexType index_type = sizeof(ImDrawIdx) == 2 ? gpu::IndexType::uint16 : gpu::IndexType::uint32;
    const float2 scale{.x = 2.0f / draw_data->DisplaySize.x, .y = 2.0f / draw_data->DisplaySize.y};
    const float2 translate{.x = -1.0f - draw_data->DisplayPos.x * scale.x, .y = -1.0f - draw_data->DisplayPos.y * scale.y};
    bind_render_state(*data, commands, framebuffer_width, framebuffer_height);
    data->sampler = 0;

    // The vertex pointer carries the vertex offset, because SV_VertexID excludes the base vertex of draw_indexed.
    const ImGuiNgaRoot* root = nullptr;
    uint32 root_texture = 0; // ImTextureID_Invalid, which no draw uses.
    uint32 root_sampler = 0;
    const ImGuiNgaVertex* root_vertices = nullptr;
    uint32 vertex_base = 0;
    uint32 index_base = 0;
    gpu::Scissor scissor{.x = -1}; // No clip rect maps to x = -1, so the first draw, and the first after a user callback, sets its scissor.
    for (const ImDrawList* list : draw_data->CmdLists)
    {
        for (const ImDrawCmd& command : list->CmdBuffer)
        {
            if (command.UserCallback)
            {
                if (command.UserCallback == reset_render_state)
                {
                    bind_render_state(*data, commands, framebuffer_width, framebuffer_height);
                    data->sampler = 0;
                }
                else if (command.UserCallback == set_sampler_linear || command.UserCallback == set_sampler_nearest) command.UserCallback(list, &command);
                else
                {
                    command.UserCallback(list, &command);
                    scissor = {.x = -1};
                }
                continue;
            }
            const float min_x = fmaxf((command.ClipRect.x - draw_data->DisplayPos.x) * draw_data->FramebufferScale.x, 0.0f);
            const float min_y = fmaxf((command.ClipRect.y - draw_data->DisplayPos.y) * draw_data->FramebufferScale.y, 0.0f);
            const float max_x = fminf((command.ClipRect.z - draw_data->DisplayPos.x) * draw_data->FramebufferScale.x, framebuffer_width);
            const float max_y = fminf((command.ClipRect.w - draw_data->DisplayPos.y) * draw_data->FramebufferScale.y, framebuffer_height);
            if (max_x <= min_x || max_y <= min_y) continue;
            const gpu::Scissor scissor_rect{.x = int32(min_x), .y = int32(min_y), .width = uint32(max_x - min_x), .height = uint32(max_y - min_y)};
            if (scissor_rect.x != scissor.x || scissor_rect.y != scissor.y || scissor_rect.width != scissor.width || scissor_rect.height != scissor.height)
            {
                gpu::set_scissor(commands, scissor_rect);
                scissor = scissor_rect;
            }

            const uint32 texture = uint32(command.GetTexID());
            ImGuiNgaVertex* command_vertices = vertices.gpu + vertex_base + command.VtxOffset;
            if (texture != root_texture || command_vertices != root_vertices || data->sampler != root_sampler)
            {
                const gpu::GpuCpuRange<ImGuiNgaRoot> allocation = bump.allocate<ImGuiNgaRoot>();
                assert(allocation.size && "the bump allocator has no room for a root");
                *allocation.cpu = {
                    .vertices = command_vertices, .texture = texture, .srgb = data->srgb, .sampler = data->sampler, .scale = scale, .translate = translate};
                root = allocation.gpu;
                root_texture = texture;
                root_vertices = command_vertices;
                root_sampler = data->sampler;
            }
            gpu::draw_indexed(commands, root, gpu::gpu_range(indices), index_type, command.ElemCount, 1, index_base + command.IdxOffset, 0);
        }
        vertex_base += uint32(list->VtxBuffer.Size);
        index_base += uint32(list->IdxBuffer.Size);
    }
}
