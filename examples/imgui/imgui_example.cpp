#include <NoGraphicsAPIUtility/imgui_impl_nographicsapi.h>
#include <imgui_impl_sdl3.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <stdio.h>
#include <string.h>

namespace
{

constexpr gpu::Format swapchain_format = gpu::Format::bgra8_srgb;
constexpr uint32 frames_in_flight = 2;
constexpr uint64 bump_bytes = 16ull << 20; // The vertices, indices and roots of one frame, and a new font atlas.

uint32 validation_messages = 0;

// The device prints each validation message to stderr itself. Count those, and print the other diagnostics.
void report_error(const char* message, bool) noexcept
{
    if (strncmp(message, "NoGraphicsAPI validation:", 25) == 0) ++validation_messages;
    else fprintf(stderr, "%s\n", message);
}

VkSurfaceKHR_T* create_window_surface(VkInstance_T* instance, void* user) noexcept
{
    VkSurfaceKHR_T* surface = nullptr;
    if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(user), instance, nullptr, &surface))
        fprintf(stderr, "SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
    return surface;
}

gpu::uint32x2 window_drawable_extent(void* user) noexcept
{
    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(static_cast<SDL_Window*>(user), &width, &height);
    return {.x = uint32(width), .y = uint32(height)};
}

} // namespace

int main()
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("NoGraphicsAPI ImGui", 1280, 800, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    Uint32 extension_count = 0;
    const char* const* extensions = window ? SDL_Vulkan_GetInstanceExtensions(&extension_count) : nullptr;
    if (!extensions)
    {
        fprintf(stderr, "SDL window creation failed: %s\n", SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    gpu::set_error_callback(report_error);
    gpu::Device* device = gpu::create_device({
        .swapchain_format = swapchain_format,
        .instance_extensions = extensions,
        .instance_extension_count = extension_count,
        .create_surface = create_window_surface,
        .drawable_extent = window_drawable_extent,
        .wsi_user = window,
    }).device;
    if (!device)
    {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    printf("Using %s\n", gpu::get_device_caps(device).device_name);

    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(device);
    gpu::CommandPool* pools[frames_in_flight] = {gpu::create_command_pool(device), gpu::create_command_pool(device)};
    const gpu::GpuHeap staging[frames_in_flight] = {gpu::create_gpu_heap(device, bump_bytes), gpu::create_gpu_heap(device, bump_bytes)};
    gpu::BumpAllocator bumps[frames_in_flight] = {gpu::BumpAllocator(staging[0].range), gpu::BumpAllocator(staging[1].range)};

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplSDL3_InitForVulkan(window);
    const ImGui_ImplNoGraphicsAPI_InitInfo init_info{.device = device, .colorFormat = swapchain_format, .framesInFlight = frames_in_flight};
    const bool initialized = ImGui_ImplNoGraphicsAPI_Init(&init_info);

    // frame counts the submitted frames. Frame value v uses pools[(v - 1) % frames_in_flight] and bumps[(v - 1) % frames_in_flight].
    uint64 frame = 0;
    bool running = initialized;
    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
        }
        if (!running) break;
        // ImGui counts each built frame toward UnusedFrames, so build only a frame that is submitted.
        // get_drawable_extent also marks the swapchain for recreation after a resize: Wayland reports no resize through acquire.
        const gpu::uint32x2 extent = gpu::get_drawable_extent(device);
        if (!extent.x || !extent.y || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED))
        {
            SDL_Delay(10);
            continue;
        }

        const uint32 slot = uint32(frame % frames_in_flight);
        if (frame >= frames_in_flight) gpu::wait_timeline({.semaphore = timeline, .value = frame + 1 - frames_in_flight});
        gpu::reset_command_pool(pools[slot]);
        bumps[slot].reset();
        gpu::CommandBuffer* commands = gpu::begin_commands(pools[slot]);
        const gpu::SwapchainFrame swapchain = gpu::acquire(commands);
        if (!swapchain.render_view) continue;

        ImGui_ImplNoGraphicsAPI_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::ShowDemoWindow();
        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();

        ImGui_ImplNoGraphicsAPI_UpdateTextures(draw_data, commands, bumps[slot]);
        gpu::begin_render_pass(commands, {
            .colors = {{.render_view = swapchain.render_view, .load = gpu::LoadOp::clear, .clear = {.x = 0.05f, .y = 0.05f, .z = 0.05f}}},
        });
        ImGui_ImplNoGraphicsAPI_RenderDrawData(draw_data, commands, bumps[slot]);
        gpu::end_render_pass(commands);
        gpu::end_commands(commands);
        ++frame;
        gpu::submit_and_present(device, {.commands = {&commands, 1}, .completion = {.semaphore = timeline, .value = frame}});
    }

    gpu::wait_idle(device);
    if (initialized) ImGui_ImplNoGraphicsAPI_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    gpu::destroy_gpu_heap(staging[1]);
    gpu::destroy_gpu_heap(staging[0]);
    gpu::destroy_command_pool(pools[1]);
    gpu::destroy_command_pool(pools[0]);
    gpu::destroy_timeline_semaphore(timeline);
    gpu::destroy_device(device);
    SDL_DestroyWindow(window);
    SDL_Quit();

    printf("%u frames, %u validation messages\n", uint32(frame), validation_messages);
    return initialized && !validation_messages ? 0 : 1;
}
