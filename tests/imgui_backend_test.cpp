#include <NoGraphicsAPIUtility/imgui_impl_nographicsapi.h>
#include <imgui_internal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{

int check(bool ok, const char* what) noexcept
{
    if (!ok) fprintf(stderr, "FAIL: %s\n", what);
    return ok ? 0 : 1;
}

int test_init(gpu::Device* device, const char*) noexcept
{
    ImGuiIO& io = ImGui::GetIO();
    const ImGui_ImplNoGraphicsAPI_InitInfo info{.device = device, .colorFormat = gpu::Format::rgba8_unorm};
    if (!ImGui_ImplNoGraphicsAPI_Init(&info)) return check(false, "Init returns true");
    int failures = 0;
    failures += check((io.BackendFlags & ImGuiBackendFlags_RendererHasTextures) != 0, "Init sets RendererHasTextures");
    failures += check((io.BackendFlags & ImGuiBackendFlags_RendererHasVtxOffset) != 0, "Init sets RendererHasVtxOffset");
    failures += check(io.BackendRendererName && strcmp(io.BackendRendererName, "imgui_impl_nographicsapi") == 0, "Init names the backend");

    ImGui_ImplNoGraphicsAPI_NewFrame();
    io.DisplaySize = ImVec2(64.0f, 64.0f);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    ImGui::Render();
    const ImDrawData* draw_data = ImGui::GetDrawData();
    failures += check(draw_data->Textures && draw_data->Textures->Size == 1 && (*draw_data->Textures)[0]->Status == ImTextureStatus_WantCreate,
                      "the first frame asks the backend to create the font atlas");

    ImGui_ImplNoGraphicsAPI_Shutdown();
    failures += check(!io.BackendRendererName && !io.BackendRendererUserData, "Shutdown clears the backend name and data");
    failures += check((io.BackendFlags & (ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset)) == 0, "Shutdown clears the flags");
    return failures ? 1 : 0;
}

constexpr uint64 staging_bytes = 4ull << 20;
constexpr uint32 target_width = 256;
constexpr uint32 target_height = 160;
constexpr uint64 target_bytes = uint64(target_width) * target_height * 4;
constexpr gpu::ClearColor clear_color{.x = 0.25f, .y = 0.5f, .z = 0.75f, .w = 1.0f};
constexpr ImGuiWindowFlags fixed_window_flags =
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

struct Fixture
{
    gpu::Device* device = nullptr;
    gpu::CommandPool* pool = nullptr;
    gpu::GpuHeap staging{};
    gpu::TimelinePoint completion{};
};

bool create_fixture(Fixture& fixture, gpu::Device* device) noexcept
{
    fixture.device = device;
    fixture.pool = gpu::create_command_pool(device);
    fixture.staging = gpu::create_gpu_heap(device, staging_bytes);
    fixture.completion.semaphore = gpu::create_timeline_semaphore(device);
    return fixture.pool && fixture.staging.owner && fixture.completion.semaphore;
}

void destroy_fixture(Fixture& fixture) noexcept
{
    gpu::wait_idle(fixture.device);
    gpu::destroy_timeline_semaphore(fixture.completion.semaphore);
    gpu::destroy_gpu_heap(fixture.staging);
    gpu::destroy_command_pool(fixture.pool);
}

void submit_and_wait(Fixture& fixture, gpu::CommandBuffer* commands) noexcept
{
    gpu::end_commands(commands);
    ++fixture.completion.value;
    gpu::submit(fixture.device, {.commands = {commands}, .completion = fixture.completion});
    gpu::wait_timeline(fixture.completion);
}

void request_update(ImTextureData* texture, ImTextureRect rect) noexcept
{
    texture->Updates.push_back(rect);
    texture->UpdateRect = rect;
    texture->SetStatus(ImTextureStatus_WantUpdates);
}

// Starts one ImGui frame on a 256x160 display.
void new_frame() noexcept
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(target_width), float(target_height));
    io.DeltaTime = 1.0f / 60.0f;
    ImGui_ImplNoGraphicsAPI_NewFrame();
    ImGui::NewFrame();
}

// One ImGui frame: the texture requests are recorded, submitted and finished before it returns.
void run_frame(Fixture& fixture) noexcept
{
    new_frame();
    ImGui::Render();
    gpu::BumpAllocator bump(fixture.staging.range);
    gpu::reset_command_pool(fixture.pool);
    gpu::CommandBuffer* commands = gpu::begin_commands(fixture.pool);
    ImGui_ImplNoGraphicsAPI_UpdateTextures(ImGui::GetDrawData(), commands, bump);
    submit_and_wait(fixture, commands);
}

int test_lifecycle(gpu::Device* device, const char*) noexcept
{
    Fixture fixture{};
    if (!create_fixture(fixture, device))
    {
        destroy_fixture(fixture);
        return check(false, "the fixture is created");
    }
    const ImGui_ImplNoGraphicsAPI_InitInfo info{.device = device, .colorFormat = gpu::Format::rgba8_unorm, .framesInFlight = 2};
    if (!ImGui_ImplNoGraphicsAPI_Init(&info))
    {
        destroy_fixture(fixture);
        return check(false, "Init returns true");
    }
    int failures = 0;

    run_frame(fixture);
    ImTextureData* atlas = ImGui::GetIO().Fonts->TexData;
    failures += check(atlas->Status == ImTextureStatus_OK && atlas->TexID == 1, "the font atlas is created in slot 1");

    ImTextureData user;
    user.Create(ImTextureFormat_RGBA32, 4, 4);
    ImGui::RegisterUserTexture(&user);
    run_frame(fixture);
    failures += check(user.Status == ImTextureStatus_OK && user.TexID == 2, "a user texture is created in slot 2");

    request_update(&user, {.x = 1, .y = 1, .w = 2, .h = 2});
    run_frame(fixture);
    failures += check(user.Status == ImTextureStatus_OK, "an update returns the texture to OK");

    user.WantDestroyNextFrame = true;
    run_frame(fixture);
    failures += check(user.Status == ImTextureStatus_WantDestroy && user.TexID == 2, "a texture unused for one frame survives with framesInFlight 2");
    run_frame(fixture);
    failures += check(user.Status == ImTextureStatus_Destroyed && user.TexID == ImTextureID_Invalid,
                      "a texture unused for framesInFlight frames is destroyed");
    ImGui::UnregisterUserTexture(&user);

    ImTextureData second;
    second.Create(ImTextureFormat_RGBA32, 2, 2);
    ImGui::RegisterUserTexture(&second);
    ImTextureData alpha;
    alpha.Create(ImTextureFormat_Alpha8, 3, 3);
    ImGui::RegisterUserTexture(&alpha);
    run_frame(fixture);
    failures += check(second.Status == ImTextureStatus_OK && second.TexID == 2, "a new texture reuses the freed slot 2");
    failures += check(alpha.Status == ImTextureStatus_OK && alpha.TexID == 3, "an Alpha8 texture is created in slot 3");

    ImGui_ImplNoGraphicsAPI_Shutdown();
    failures += check(atlas->TexID == ImTextureID_Invalid && second.TexID == ImTextureID_Invalid && alpha.TexID == ImTextureID_Invalid,
                      "Shutdown destroys every live texture");
    ImGui::UnregisterUserTexture(&alpha);
    ImGui::UnregisterUserTexture(&second);
    destroy_fixture(fixture);
    return failures ? 1 : 0;
}

struct RenderTarget
{
    gpu::TextureHeap heap{};
    gpu::Texture* texture = nullptr;
    gpu::RenderView* view = nullptr;
    gpu::GpuHeap readback{};
};

// A 256x160 color target, initialized, and a readback heap for its bytes.
bool create_render_target(Fixture& fixture, RenderTarget& target, gpu::Format format) noexcept
{
    const gpu::TextureDesc desc{
        .extent = {.x = target_width, .y = target_height, .z = 1},
        .format = format,
        .usage = gpu::TextureUsage::color_attachment | gpu::TextureUsage::transfer_source,
    };
    target.heap = gpu::create_texture_heap(fixture.device, gpu::get_texture_size_align(fixture.device, desc).size);
    target.readback = gpu::create_gpu_heap(fixture.device, target_bytes, gpu::MemoryType::readback);
    if (!target.heap.owner || !target.readback.owner) return false;
    target.texture = gpu::create_texture(fixture.device, desc, target.heap, 0);
    if (!target.texture) return false;
    target.view = gpu::create_render_view(target.texture);
    if (!target.view) return false;
    gpu::reset_command_pool(fixture.pool);
    gpu::CommandBuffer* commands = gpu::begin_commands(fixture.pool);
    gpu::initialize_texture(commands, target.texture);
    submit_and_wait(fixture, commands);
    return true;
}

void destroy_render_target(RenderTarget& target) noexcept
{
    gpu::destroy_render_view(target.view);
    gpu::destroy_texture(target.texture);
    gpu::destroy_texture_heap(target.heap);
    gpu::destroy_gpu_heap(target.readback);
}

// Creates the fixture and the target and initializes the backend for format. On a failure it destroys what it created and returns false.
bool begin_case(gpu::Device* device, Fixture& fixture, RenderTarget& target, gpu::Format format) noexcept
{
    const ImGui_ImplNoGraphicsAPI_InitInfo info{.device = device, .colorFormat = format};
    if (create_fixture(fixture, device) && create_render_target(fixture, target, format) && ImGui_ImplNoGraphicsAPI_Init(&info)) return true;
    destroy_render_target(target);
    destroy_fixture(fixture);
    return false;
}

// The command buffer that render_frame records, for user callbacks.
gpu::CommandBuffer* frame_commands = nullptr;

// Ends the ImGui frame that new_frame began and draws it into the target, cleared to clear_color. The work is finished before it
// returns. With read_back, target.readback holds the bytes of the target afterwards.
void render_frame(Fixture& fixture, RenderTarget& target, bool read_back) noexcept
{
    ImGui::Render();
    ImDrawData* draw_data = ImGui::GetDrawData();
    gpu::BumpAllocator bump(fixture.staging.range);
    gpu::reset_command_pool(fixture.pool);
    gpu::CommandBuffer* commands = gpu::begin_commands(fixture.pool);
    frame_commands = commands;
    ImGui_ImplNoGraphicsAPI_UpdateTextures(draw_data, commands, bump);
    const gpu::ColorAttachment color{.render_view = target.view, .load = gpu::LoadOp::clear, .clear = clear_color};
    gpu::begin_render_pass(commands, {.colors = {&color, 1}});
    ImGui_ImplNoGraphicsAPI_RenderDrawData(draw_data, commands, bump);
    gpu::end_render_pass(commands);
    if (read_back)
    {
        gpu::barrier(commands, gpu::Stage::color_output, gpu::Access::color_write, gpu::Stage::transfer, gpu::Access::transfer_read);
        gpu::copy_texture_to_memory(commands, target.texture, gpu::gpu_range(target.readback));
        gpu::barrier(commands, gpu::Stage::transfer, gpu::Access::transfer_write, gpu::Stage::host, gpu::Access::host_read);
    }
    submit_and_wait(fixture, commands);
}

const uint8* pixel_at(const RenderTarget& target, uint32 x, uint32 y) noexcept
{
    return target.readback.range.cpu + (uint64(y) * target_width + x) * 4;
}

// True when a channel of the pixel (x, y) of the read-back target differs by more than 8 from the pixel (reference_x, reference_y).
bool pixel_differs(const RenderTarget& target, uint32 x, uint32 y, uint32 reference_x, uint32 reference_y) noexcept
{
    const uint8* pixel = pixel_at(target, x, y);
    const uint8* reference = pixel_at(target, reference_x, reference_y);
    for (uint32 channel = 0; channel < 4; ++channel)
        if (abs(int(pixel[channel]) - int(reference[channel])) > 8) return true;
    return false;
}

// Without golden_output, the third frame is compared with golden/imgui_backend.rgba, relative to the working directory.
// With golden_output, the third frame is written to that path instead.
int test_golden(gpu::Device* device, const char* golden_output) noexcept
{
    // An Alpha8 atlas draws the same pixels as the default RGBA32 one, and takes the Alpha8 path of the texture upload.
    ImGui::GetIO().Fonts->TexDesiredFormat = ImTextureFormat_Alpha8;
    Fixture fixture{};
    RenderTarget target{};
    if (!begin_case(device, fixture, target, gpu::Format::rgba8_unorm)) return check(false, "the fixture, the render target and the backend are created");
    int failures = 0;

    // Four columns: red, green, blue and white. After the first frame, an update paints the center 2x2 texels yellow.
    ImTextureData user;
    user.Create(ImTextureFormat_RGBA32, 4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
        {
            uint8* pixel = static_cast<uint8*>(user.GetPixelsAt(x, y));
            pixel[0] = x == 0 || x == 3 ? 255 : 0;
            pixel[1] = x == 1 || x == 3 ? 255 : 0;
            pixel[2] = x == 2 || x == 3 ? 255 : 0;
            pixel[3] = 255;
        }
    ImGui::RegisterUserTexture(&user);

    float slider = 0.5f;
    for (uint32 frame = 0; frame < 3; ++frame)
    {
        new_frame();
        ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f));
        ImGui::SetNextWindowSize(ImVec2(240.0f, 144.0f));
        ImGui::Begin("Golden", nullptr, fixed_window_flags);
        ImGui::Text("NoGraphicsAPI");
        ImGui::Button("Button");
        ImGui::SliderFloat("Slider", &slider, 0.0f, 1.0f);
        ImGui::ColorButton("Color", ImVec4(1.0f, 0.5f, 0.0f, 1.0f));
        ImGui::SameLine();
        ImGui::Image(user.GetTexRef(), ImVec2(32.0f, 32.0f));
        ImGui::End();
        render_frame(fixture, target, frame == 2);
        if (frame == 0)
        {
            for (int y = 1; y < 3; ++y)
                for (int x = 1; x < 3; ++x)
                {
                    uint8* pixel = static_cast<uint8*>(user.GetPixelsAt(x, y));
                    pixel[0] = 255;
                    pixel[1] = 255;
                    pixel[2] = 0;
                }
            request_update(&user, {.x = 1, .y = 1, .w = 2, .h = 2});
        }
    }
    failures += check(ImGui::GetIO().Fonts->TexData->Format == ImTextureFormat_Alpha8, "the font atlas of the golden is Alpha8");
    failures += check(user.Status == ImTextureStatus_OK, "the user texture update is done");

    if (golden_output)
    {
        FILE* file = fopen(golden_output, "wb");
        const bool written = file && fwrite(target.readback.range.cpu, 1, target_bytes, file) == target_bytes;
        if (file) fclose(file);
        failures += check(written, "the third frame is written to the golden output path");
    }
    else
    {
        FILE* file = fopen("golden/imgui_backend.rgba", "rb");
        uint8 row[target_width * 4];
        uint32 rows = 0;
        uint32 differing = 0;
        for (; file && rows < target_height && fread(row, 1, sizeof(row), file) == sizeof(row); ++rows)
            for (uint32 index = 0; index < sizeof(row); ++index)
                differing += abs(int(row[index]) - int(target.readback.range.cpu[rows * sizeof(row) + index])) > 8;
        if (file) fclose(file);
        if (differing) fprintf(stderr, "%u channels differ from the golden by more than 8\n", differing);
        failures += check(rows == target_height, "golden/imgui_backend.rgba holds 256x160 RGBA8 pixels");
        failures += check(differing == 0, "the third frame matches the golden within 8");
    }

    ImGui_ImplNoGraphicsAPI_Shutdown();
    ImGui::UnregisterUserTexture(&user);
    destroy_render_target(target);
    destroy_fixture(fixture);
    return failures ? 1 : 0;
}

// Counts its calls and leaves a 1x1 viewport, which the reset callback that follows it must replace.
void count_callback(const ImDrawList*, const ImDrawCmd* command) noexcept
{
    ++*static_cast<uint32*>(command->UserCallbackData);
    gpu::set_viewport(frame_commands, {.width = 1.0f, .height = 1.0f});
}

int test_scissor(gpu::Device* device, const char*) noexcept
{
    Fixture fixture{};
    RenderTarget target{};
    if (!begin_case(device, fixture, target, gpu::Format::rgba8_unorm)) return check(false, "the fixture, the render target and the backend are created");
    int failures = 0;
    failures += check(ImGui::GetPlatformIO().DrawCallback_ResetRenderState != nullptr, "Init sets DrawCallback_ResetRenderState");

    // The first window reaches past the top-left corner. It holds a rect whose clip rect exceeds the target on every side, and a circle
    // whose clip rect lies outside the target. The circle gives the first draw list more vertices than the second one indexes, so a
    // draw that ignores the vertex base of the second list draws only geometry of the first. The second window reaches past the
    // bottom-right corner.
    uint32 callback_count = 0;
    for (uint32 frame = 0; frame < 3; ++frame)
    {
        new_frame();
        ImGui::SetNextWindowPos(ImVec2(-40.0f, -30.0f));
        ImGui::SetNextWindowSize(ImVec2(120.0f, 90.0f));
        ImGui::Begin("Top left", nullptr, fixed_window_flags);
        ImGui::Text("Top left");
        ImDrawList* list = ImGui::GetWindowDrawList();
        list->PushClipRect(ImVec2(-64.0f, -64.0f), ImVec2(320.0f, 224.0f));
        list->AddRectFilled(ImVec2(100.0f, 60.0f), ImVec2(150.0f, 90.0f), IM_COL32(255, 0, 255, 255));
        list->PopClipRect();
        list->PushClipRect(ImVec2(300.0f, 200.0f), ImVec2(340.0f, 240.0f));
        list->AddCircleFilled(ImVec2(320.0f, 220.0f), 16.0f, IM_COL32(255, 0, 255, 255), 64);
        list->PopClipRect();
        // Past 65536 vertices ImGui starts a command with a new VtxOffset, so the last rect draws only through that offset.
        list->PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(256.0f, 160.0f));
        for (uint32 index = 0; index < 16400; ++index) list->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), IM_COL32(255, 0, 255, 255));
        list->AddRectFilled(ImVec2(100.0f, 130.0f), ImVec2(120.0f, 150.0f), IM_COL32(255, 0, 255, 255));
        list->PopClipRect();
        ImGui::End();
        ImGui::SetNextWindowPos(ImVec2(176.0f, 100.0f));
        ImGui::SetNextWindowSize(ImVec2(120.0f, 90.0f));
        ImGui::Begin("Bottom right", nullptr, fixed_window_flags);
        ImGui::GetWindowDrawList()->AddCallback(count_callback, &callback_count);
        ImGui::GetWindowDrawList()->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
        ImGui::Text("Bottom right");
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(220.0f, 140.0f), ImVec2(250.0f, 158.0f), IM_COL32(255, 0, 255, 255));
        ImGui::End();
        render_frame(fixture, target, frame == 2);
    }
    // (30, 130) is covered by nothing, so it holds the clear color.
    failures += check(pixel_differs(target, 20, 20, 30, 130), "the window off the top-left corner is drawn");
    failures += check(pixel_differs(target, 230, 140, 30, 130), "the window off the bottom-right corner is drawn");
    failures += check(pixel_differs(target, 125, 75, 30, 130), "a rect whose clip rect exceeds the target is drawn");
    failures += check(pixel_differs(target, 240, 150, 185, 155), "the reset callback restores the viewport after a user callback");
    failures += check(pixel_differs(target, 110, 140, 30, 130), "a rect past 65536 vertices of one draw list is drawn");
    failures += check(callback_count == 3, "RenderDrawData calls the user callback once in each frame");

    ImGui_ImplNoGraphicsAPI_Shutdown();
    failures += check(ImGui::GetPlatformIO().DrawCallback_ResetRenderState == nullptr, "Shutdown clears DrawCallback_ResetRenderState");
    destroy_render_target(target);
    destroy_fixture(fixture);
    return failures ? 1 : 0;
}

// One opaque quad, drawn with the white texel of the font atlas into an rgba8_srgb target. The backend converts the vertex color to
// linear and the target converts it back, so the target holds the bytes of the vertex color.
int test_srgb(gpu::Device* device, const char*) noexcept
{
    Fixture fixture{};
    RenderTarget target{};
    if (!begin_case(device, fixture, target, gpu::Format::rgba8_srgb)) return check(false, "the fixture, the render target and the backend are created");

    // Mid values, because 0 and 255 read back the same with and without the conversion.
    const uint8 color[4] = {64, 128, 192, 255};
    for (uint32 frame = 0; frame < 2; ++frame)
    {
        new_frame();
        ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(32.0f, 32.0f), ImVec2(224.0f, 128.0f), IM_COL32(color[0], color[1], color[2], color[3]));
        render_frame(fixture, target, frame == 1);
    }
    // (128, 80) is the center of the quad.
    const uint8* pixel = pixel_at(target, 128, 80);
    bool matches = true;
    for (uint32 channel = 0; channel < 4; ++channel) matches = matches && abs(int(pixel[channel]) - int(color[channel])) <= 1;
    if (!matches)
        fprintf(stderr, "read back %d %d %d %d for the vertex color %d %d %d %d\n", pixel[0], pixel[1], pixel[2], pixel[3], color[0], color[1], color[2],
                color[3]);
    const int failures = check(matches, "an rgba8_srgb target reads back the vertex color within 1");

    ImGui_ImplNoGraphicsAPI_Shutdown();
    destroy_render_target(target);
    destroy_fixture(fixture);
    return failures;
}

struct TestCase
{
    const char* name;
    int (*run)(gpu::Device* device, const char* golden_output) noexcept;
};

const TestCase test_cases[] = {
    {.name = "init", .run = test_init},
    {.name = "lifecycle", .run = test_lifecycle},
    {.name = "golden", .run = test_golden},
    {.name = "scissor", .run = test_scissor},
    {.name = "srgb", .run = test_srgb},
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3)
    {
        fprintf(stderr, "usage: test_imgui_backend <case> [golden_output]\n");
        return 2;
    }
    const TestCase* selected = nullptr;
    for (const TestCase& test : test_cases)
        if (strcmp(test.name, argv[1]) == 0) selected = &test;
    if (!selected)
    {
        fprintf(stderr, "unknown case: %s\n", argv[1]);
        return 2;
    }
    const gpu::DeviceInit initialized = gpu::create_device({});
    if (initialized.error == gpu::Error::unsupported) return 77;
    if (initialized.error != gpu::Error::none) return 1;
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    const int result = selected->run(initialized.device, argc == 3 ? argv[2] : nullptr);
    ImGui::DestroyContext();
    gpu::wait_idle(initialized.device);
    gpu::destroy_device(initialized.device);
    printf("ImGui backend %s: %s\n", argv[1], result ? "failed" : "passed");
    return result;
}
