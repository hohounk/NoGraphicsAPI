#include <NoGraphicsAPIUtility/imgui_impl_nographicsapi.h>

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

struct TestCase
{
    const char* name;
    int (*run)(gpu::Device* device, const char* golden_output) noexcept;
};

const TestCase test_cases[] = {
    {.name = "init", .run = test_init},
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
