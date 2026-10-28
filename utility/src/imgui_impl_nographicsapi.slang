#include <NoGraphicsAPI/shader.slang>
#include "imgui_impl_nographicsapi_shared.h"

GPU_ROOT(ImGuiNgaRoot, root);

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

[shader("vertex")]
VertexOutput vertexMain(uint vertex_id : SV_VertexID)
{
    ImGuiNgaVertex vertex = root.vertices[vertex_id];
    float4 color = float4(uint4(vertex.color, vertex.color >> 8, vertex.color >> 16, vertex.color >> 24) & 0xffu) / 255.0f;
    if (root.srgb != 0) color.rgb = select(color.rgb <= 0.04045f, color.rgb / 12.92f, pow((color.rgb + 0.055f) / 1.055f, 2.4f));

    VertexOutput output;
    output.position = float4(vertex.position * root.scale + root.translate, 0.0f, 1.0f);
    output.uv = vertex.uv;
    output.color = color;
    return output;
}

[shader("fragment")]
float4 fragmentMain(VertexOutput input) : SV_Target0
{
    Texture2D<float4> texture = gpu_texture<Texture2D<float4>>(root.texture);
    return input.color * texture.Sample(gpu_sampler(root.sampler), input.uv);
}
