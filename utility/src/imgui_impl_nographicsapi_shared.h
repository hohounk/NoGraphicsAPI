#pragma once

#include <NoGraphicsAPIUtility/shader_types.h>

// The layout of ImDrawVert: position, uv, packed RGBA8 color.
struct ImGuiNgaVertex
{
    float2 position;
    float2 uv;
    uint32 color;
};

struct ImGuiNgaRoot
{
    ImGuiNgaVertex* vertices; // Already offset by ImDrawCmd::VtxOffset: SV_VertexID excludes the base vertex.
    uint32 texture;
    uint32 srgb;
    float2 scale;
    float2 translate;
};

#if !defined(__SLANG__)
static_assert(sizeof(ImGuiNgaVertex) == 20);
static_assert(sizeof(ImGuiNgaRoot) == 32);
#endif
