#include <NoGraphicsAPIUtility/imgui_impl_nographicsapi.h>
#include <imgui_internal.h>

#include <stdio.h>
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
    fixture = {};
}

// One ImGui frame: the texture requests are recorded, submitted and finished before it returns.
void run_frame(Fixture& fixture) noexcept
{
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(256.0f, 160.0f);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui_ImplNoGraphicsAPI_NewFrame();
    ImGui::NewFrame();
    ImGui::Render();
    gpu::BumpAllocator bump(fixture.staging.range);
    gpu::reset_command_pool(fixture.pool);
    gpu::CommandBuffer* commands = gpu::begin_commands(fixture.pool);
    ImGui_ImplNoGraphicsAPI_UpdateTextures(ImGui::GetDrawData(), commands, bump);
    gpu::end_commands(commands);
    ++fixture.completion.value;
    gpu::submit(fixture.device, {.commands = {commands}, .completion = fixture.completion});
    gpu::wait_timeline(fixture.completion);
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

    const ImTextureRect rect{.x = 1, .y = 1, .w = 2, .h = 2};
    user.Updates.push_back(rect);
    user.UpdateRect = rect;
    user.SetStatus(ImTextureStatus_WantUpdates);
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

struct TestCase
{
    const char* name;
    int (*run)(gpu::Device* device, const char* golden_output) noexcept;
};

const TestCase test_cases[] = {
    {.name = "init", .run = test_init},
    {.name = "lifecycle", .run = test_lifecycle},
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
