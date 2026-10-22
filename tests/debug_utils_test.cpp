#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
struct NameRecord
{
    VkObjectType type;
    uint64 handle;
    char name[64];
};
NameRecord names[32]{};
uint32 name_count = 0;
PFN_vkSetDebugUtilsObjectNameEXT next_set_name = nullptr;
bool hide_debug_utils = false;
uint32 failures = 0;

VKAPI_ATTR VkResult VKAPI_CALL record_set_name(VkDevice device, const VkDebugUtilsObjectNameInfoEXT* info)
{
    if (name_count < 32)
    {
        names[name_count] = {.type = info->objectType, .handle = info->objectHandle};
        snprintf(names[name_count].name, sizeof(names[name_count].name), "%s", info->pObjectName);
        ++name_count;
    }
    return next_set_name(device, info);
}

PFN_vkVoidFunction VKAPI_CALL test_vkGetDeviceProcAddr(VkDevice device, const char* name)
{
    const PFN_vkVoidFunction proc = vkGetDeviceProcAddr(device, name);
    if (proc && strcmp(name, "vkSetDebugUtilsObjectNameEXT") == 0)
    {
        next_set_name = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(proc);
        return reinterpret_cast<PFN_vkVoidFunction>(record_set_name);
    }
    return proc;
}

VkResult VKAPI_CALL test_vkEnumerateInstanceExtensionProperties(const char* layer, uint32* count, VkExtensionProperties* values)
{
    const VkResult result = vkEnumerateInstanceExtensionProperties(layer, count, values);
    if (hide_debug_utils && values && result == VK_SUCCESS)
    {
        uint32 written = 0;
        for (uint32 index = 0; index < *count; ++index)
            if (strcmp(values[index].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) values[written++] = values[index];
        *count = written;
    }
    return result;
}
} // namespace

#define vkGetDeviceProcAddr test_vkGetDeviceProcAddr
#define vkEnumerateInstanceExtensionProperties test_vkEnumerateInstanceExtensionProperties
#include "../src/NoGraphicsAPI.cpp"

namespace
{
void check(bool condition, const char* message) noexcept
{
    if (condition) return;
    fprintf(stderr, "FAILED: %s\n", message);
    ++failures;
}

bool named(VkObjectType type, uint64 handle, const char* name) noexcept
{
    for (uint32 index = 0; index < name_count; ++index)
        if (names[index].type == type && names[index].handle == handle && strcmp(names[index].name, name) == 0) return true;
    return false;
}

uint64 handle(const void* object) noexcept
{
    return uint64(reinterpret_cast<uintptr_t>(object));
}

gpu::ByteSpan read_shader(const char* path) noexcept
{
    FILE* file = fopen(path, "rb");
    check(file != nullptr, "compute shader fixture is available");
    if (!file) return {};
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);
    byte* data = static_cast<byte*>(malloc(size_t(size)));
    const bool valid = size > 0 && fread(data, 1, size_t(size), file) == size_t(size);
    fclose(file);
    check(valid, "compute shader fixture loads completely");
    if (!valid) { free(data); return {}; }
    return {data, uint64(size)};
}

void object_names(gpu::Device* device) noexcept
{
    const gpu::GpuHeap gpu_heap = gpu::create_gpu_heap(device, 4096);
    const gpu::TextureDesc texture_desc{.extent = {.x = 16, .y = 16, .z = 1}, .usage = gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment};
    const gpu::SizeAlign size_align = gpu::get_texture_size_align(device, texture_desc);
    const gpu::TextureHeap texture_heap = gpu::create_texture_heap(device, size_align.size);
    gpu::Texture* texture = gpu::create_texture(device, texture_desc, texture_heap, 0);
    gpu::RenderView* render_view = gpu::create_render_view(texture);
    gpu::TextureDescriptorHeap* texture_descriptors = gpu::create_texture_descriptor_heap(device, 4);
    gpu::SamplerDescriptorHeap* sampler_descriptors = gpu::create_sampler_descriptor_heap(device, 4);
    const gpu::ByteSpan code = read_shader(NOGRAPHICSAPI_DEBUG_UTILS_COMPUTE);
    gpu::PSO* pso = code.data ? gpu::create_compute_pso(device, {.code = code, .entry_point = "computeMain"}) : nullptr;
    check(gpu_heap.owner && texture_heap.owner && texture && render_view && texture_descriptors && sampler_descriptors && pso, "test objects are created");
    if (!gpu_heap.owner || !texture_heap.owner || !texture || !render_view || !texture_descriptors || !sampler_descriptors || !pso) return;

    gpu::set_debug_name(gpu_heap, "gpu heap");
    gpu::set_debug_name(texture_heap, "texture heap");
    gpu::set_debug_name(texture, "texture");
    gpu::set_debug_name(render_view, "render view");
    gpu::set_debug_name(texture_descriptors, "texture descriptors");
    gpu::set_debug_name(sampler_descriptors, "sampler descriptors");
    gpu::set_debug_name(pso, "pso");

    check(named(VK_OBJECT_TYPE_BUFFER, handle(gpu_heap.owner->backing.buffer), "gpu heap"), "GPU heap buffer is named");
    check(named(VK_OBJECT_TYPE_DEVICE_MEMORY, handle(gpu_heap.owner->backing.memory), "gpu heap"), "GPU heap memory is named");
    check(named(VK_OBJECT_TYPE_DEVICE_MEMORY, handle(texture_heap.owner->memory), "texture heap"), "texture heap memory is named");
    check(named(VK_OBJECT_TYPE_IMAGE, handle(texture->image), "texture"), "texture image is named");
    check(named(VK_OBJECT_TYPE_IMAGE_VIEW, handle(render_view->view), "render view"), "render view is named");
    check(named(VK_OBJECT_TYPE_BUFFER, handle(texture_descriptors->storage.owner->backing.buffer), "texture descriptors"), "texture descriptor heap is named");
    check(named(VK_OBJECT_TYPE_BUFFER, handle(sampler_descriptors->storage.owner->backing.buffer), "sampler descriptors"), "sampler descriptor heap is named");
    check(named(VK_OBJECT_TYPE_PIPELINE, handle(pso->pso), "pso"), "PSO is named");

    gpu::destroy_pso(pso);
    free(const_cast<byte*>(code.data));
    gpu::destroy_sampler_descriptor_heap(sampler_descriptors);
    gpu::destroy_texture_descriptor_heap(texture_descriptors);
    gpu::destroy_render_view(render_view);
    gpu::destroy_texture(texture);
    gpu::destroy_texture_heap(texture_heap);
    gpu::destroy_gpu_heap(gpu_heap);
}

void names_without_extension() noexcept
{
    hide_debug_utils = true;
    const gpu::DeviceInit init = gpu::create_device();
    hide_debug_utils = false;
    check(init.device != nullptr, "device creation succeeds without VK_EXT_debug_utils");
    if (!init.device) return;
    name_count = 0;
    const gpu::GpuHeap heap = gpu::create_gpu_heap(init.device, 4096);
    gpu::set_debug_name(heap, "hidden");
    check(name_count == 0, "names are ignored without VK_EXT_debug_utils");
    gpu::destroy_gpu_heap(heap);
    gpu::wait_idle(init.device);
    gpu::destroy_device(init.device);
}
} // namespace

int main()
{
    const gpu::DeviceInit init = gpu::create_device();
    if (init.error == gpu::Error::unsupported)
    {
        printf("SKIP: required Vulkan device features are unavailable.\n");
        return 77;
    }
    check(init.device != nullptr, "device creation succeeds");
    if (!init.device) return 1;
    object_names(init.device);
    gpu::wait_idle(init.device);
    gpu::destroy_device(init.device);
    names_without_extension();
    printf("Debug utils: %u failures\n", failures);
    return failures ? 1 : 0;
}
