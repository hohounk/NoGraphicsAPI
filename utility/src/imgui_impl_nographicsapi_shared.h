#pragma once

#include <NoGraphicsAPIUtility/shader_types.h>

struct ImGuiNgaVertex
{
    float2 position;
    float2 uv;
    uint32 color;
};

struct ImGuiNgaRoot
{
    ImGuiNgaVertex* vertices;
    uint32 texture;
    uint32 srgb;
    float2 scale;
    float2 translate;
};
