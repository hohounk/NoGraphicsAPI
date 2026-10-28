# Dear ImGui renderer backend

`NoGraphicsAPI::ImGuiBackend` draws [Dear ImGui](https://github.com/ocornut/imgui) with NoGraphicsAPI.
The API is in [`imgui_impl_nographicsapi.h`](../utility/include/NoGraphicsAPIUtility/imgui_impl_nographicsapi.h).
The backend sets `ImGuiBackendFlags_RendererHasTextures` and `ImGuiBackendFlags_RendererHasVtxOffset`.

## Build

The CMake option `NOGRAPHICSAPI_BUILD_IMGUI_BACKEND` builds the target. It is `OFF` by default.
The backend links the imgui target that the cache variable `NOGRAPHICSAPI_IMGUI_TARGET` names. The default is `imgui`.
Define that target before you add NoGraphicsAPI. If the target does not exist, configure fails.
The tests and examples of this repository fetch Dear ImGui v1.93.0-docking when the target does not exist.
The backend is Vulkan only, because it embeds SPIR-V. With the option `ON`, configure fails on Apple platforms.
Do not define `IMGUI_USE_BGRA_PACKED_COLOR`. The shader reads `ImDrawVert::col` as RGBA8, and the backend does not compile with that define.

## Frame

Call `ImGui_ImplNoGraphicsAPI_Init` once, before the first frame. You must set `InitInfo::device` and `InitInfo::colorFormat`.
`maxTextures` and `framesInFlight` must not be zero. Init asserts each of these conditions.
`ImGui_ImplNoGraphicsAPI_Init` creates the PSO for `InitInfo::colorFormat`, the descriptor heaps and the sampler.
In each frame:

1. Call `ImGui_ImplNoGraphicsAPI_NewFrame`, the `NewFrame` of your platform backend, and `ImGui::NewFrame`.
2. Build the UI and call `ImGui::Render`.
3. Outside a render pass, call `ImGui_ImplNoGraphicsAPI_UpdateTextures` with `ImGui::GetDrawData()`.
   It creates and destroys textures at once, and records their initialization and uploads. Vulkan does not allow copies in a render pass.
4. In a render pass on a color target of `InitInfo::colorFormat`, call `ImGui_ImplNoGraphicsAPI_RenderDrawData` with the same draw data.
   It records one `draw_indexed` for each `ImDrawCmd`.
5. Submit the command buffer.

At shutdown, call `wait_idle`, then `ImGui_ImplNoGraphicsAPI_Shutdown`. Shutdown destroys every texture that ImGui still holds.

## Frames in flight

Set `InitInfo::framesInFlight` to the largest number of frames that the GPU executes at the same time. The default is 2.
When ImGui asks to destroy a texture, the backend destroys it after `framesInFlight` frames that do not use it.
Build an ImGui frame, from `ImGui::NewFrame` to `RenderDrawData`, only for a frame that you submit.
For example, do not build a frame while the window is minimized. ImGui counts each built frame toward `ImTextureData::UnusedFrames`.
A frame that you build but do not submit can thus destroy a texture that an earlier frame still reads.

## Bump allocator budget

Both record calls take their bytes from your [`gpu::BumpAllocator`](../utility/include/NoGraphicsAPIUtility/bump_allocator.hpp).
Do not reset or reuse that memory until the GPU completes the frame.

| Call | Bytes |
| --- | --- |
| `UpdateTextures` | `width * height * 4` for each new texture, and `w * h * 4` for each rect of a texture update |
| `RenderDrawData` | `TotalVtxCount * 20` of vertices, `TotalIdxCount * sizeof(ImDrawIdx)` of indices, and 32 for each root |

Each allocation rounds up to 16 bytes. The largest single need is a new font atlas.
When ImGui grows the atlas, it asks for a new texture of the larger size.
An allocation that does not fit is a programmer error, and the backend asserts.

## Textures and root data

`ImTextureID` is the slot of the texture in the texture descriptor heap of the backend.
Slot 0 stays empty, because `ImTextureID_Invalid` is 0. The heap has `InitInfo::maxTextures + 1` slots.
`maxTextures` counts the font atlas and the user textures, plus one more atlas. The default is 64.
When ImGui grows the atlas, the old atlas keeps its slot for `framesInFlight` frames while the new atlas takes a second slot.
More live textures than `maxTextures` is a programmer error, and the backend asserts.
The backend creates each texture as `rgba8_unorm`, and expands an `ImTextureFormat_Alpha8` texture to white with that alpha.
To show your own image, put its pixels in an `ImTextureData` and register it with `ImGui::RegisterUserTexture`.
The backend has no call that adds a `gpu::Texture` that you create.

The draws read a 32-byte root, [`ImGuiNgaRoot`](../utility/src/imgui_impl_nographicsapi_shared.h).
It holds a GPU pointer to the vertices, the texture slot, the sRGB flag, and the scale and translation of the display.
A new root starts when the texture or the vertex pointer changes, so a frame uses at most one root for each `ImDrawCmd`.
The vertex shader reads the vertices through the pointer, with no vertex input state.
The pointer includes `ImDrawCmd::VtxOffset`, because `SV_VertexID` excludes the base vertex of `draw_indexed`.

## Render state

`RenderDrawData` sets the viewport to the framebuffer size. It binds the backend PSO and the texture and sampler descriptor heaps of the backend.
It sets the scissor of each draw command. These bindings stay after the call.
Bind your own PSO, heaps, viewport and scissor again before you record more draws in the same command buffer.
A user callback of `ImDrawList::AddCallback` runs during the call. If the callback changes the state,
add `ImGui::GetPlatformIO().DrawCallback_ResetRenderState` after it. This callback binds the state of the backend again.

## sRGB targets

ImGui colors are sRGB values. With a `colorFormat` of `r8_srgb`, `rg8_srgb`, `rgba8_srgb` or `bgra8_srgb`,
the vertex shader converts the vertex colors to linear.
The target converts them back when it writes, so the pixels have the ImGui color values.
Only these four formats get the conversion. Other formats, float formats such as `rgba16_float` included, get the sRGB values unchanged.
On a linear target, for example an `rgba16_float` swapchain in the extended sRGB linear color space, the colors thus show lighter than in ImGui.
The conversion applies to vertex colors only. On an `_srgb` target, an `ImGui::Image` shows lighter than its texel bytes,
because the backend samples textures as `rgba8_unorm`.

## Shader

The shader is [`imgui_impl_nographicsapi.slang`](../utility/src/imgui_impl_nographicsapi.slang).
`utility/src/imgui_impl_nographicsapi_spirv.h` embeds its SPIR-V, so a build needs no shader tools.
After you change the shader, run [`tools/write_imgui_spirv.py`](../tools/write_imgui_spirv.py) to write the header again.
It uses `slangc` and `spirv-val` from `PATH`, or the paths of `--slangc` and `--spirv-val`.
The test `test_imgui_spirv_embedded` runs the script with `--check`. The test fails when the header differs from a fresh compile.
It skips when `slangc` is not the version that the header records.
