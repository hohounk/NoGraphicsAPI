#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <NoGraphicsAPIUtility/texture_allocator.hpp>
#include <NoGraphicsAPIUtility/upload_queue.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#endif
#if defined(NOGRAPHICSAPI_TEST_WAYLAND)
#include <wayland-client.h>
#endif
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
struct NativeResources
{
    int32 instances, devices, buffers, memory, semaphores, images, views, pools, queries, pipelines;
};
NativeResources live{};
thread_local const char* failure_operation = nullptr;
thread_local VkResult failure_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
thread_local uint32 failure_skip = 0;
thread_local gpu::Span<const char* const> advertised_device_extensions{};
thread_local gpu::Span<const char* const> advertised_instance_extensions{};
thread_local bool reject_first_device_only = false;
thread_local bool hide_address_commands = false;
thread_local uint32 device_extension_queries = 0;
uint32 command_allocations = 0, command_frees = 0;
uint32 failures = 0, reports = 0;
bool reported_fatal = false;
char diagnostic[2048]{};
uint32 instance_layers = 0;
bool hide_khr_swapchain_maintenance1 = false;
uint32 hidden_extensions = 0;
char instance_extension_names[16][VK_MAX_EXTENSION_NAME_SIZE]{};
uint32 instance_extension_count = 0;

VkResult enumerate_extensions(gpu::Span<const char* const> names, uint32* count, VkExtensionProperties* values) noexcept
{
    if (!values) { *count = uint32(names.size); return VK_SUCCESS; }
    const uint32 written = *count < names.size ? *count : uint32(names.size);
    for (uint32 index = 0; index < written; ++index)
    {
        values[index] = {};
        snprintf(values[index].extensionName, sizeof(values[index].extensionName), "%s", names.data[index]);
    }
    *count = written;
    return written == names.size ? VK_SUCCESS : VK_INCOMPLETE;
}

VkResult VKAPI_CALL test_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physical, const char* layer, uint32* count, VkExtensionProperties* values)
{
    if (advertised_device_extensions.data && (!reject_first_device_only || device_extension_queries++ == 0))
        return enumerate_extensions(advertised_device_extensions, count, values);
    const VkResult result = vkEnumerateDeviceExtensionProperties(physical, layer, count, values);
    if (values && result == VK_SUCCESS && (hide_address_commands || hide_khr_swapchain_maintenance1))
    {
        uint32 written = 0;
        for (uint32 index = 0; index < *count; ++index)
        {
            const bool drop_address = hide_address_commands &&
                !strcmp(values[index].extensionName, VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME);
            const bool drop_maintenance = hide_khr_swapchain_maintenance1 &&
                !strcmp(values[index].extensionName, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
            if (drop_maintenance) ++hidden_extensions;
            if (!drop_address && !drop_maintenance) values[written++] = values[index];
        }
        *count = written;
    }
    return result;
}

VkResult VKAPI_CALL test_vkEnumerateInstanceExtensionProperties(const char* layer, uint32* count, VkExtensionProperties* values)
{
    return advertised_instance_extensions.data ? enumerate_extensions(advertised_instance_extensions, count, values)
        : vkEnumerateInstanceExtensionProperties(layer, count, values);
}

VkResult VKAPI_CALL test_vkEnumeratePhysicalDevices(VkInstance instance, uint32* count, VkPhysicalDevice* values)
{
    const uint32 capacity = *count;
    const VkResult result = vkEnumeratePhysicalDevices(instance, count, values);
    if (reject_first_device_only && result == VK_SUCCESS && values && *count && capacity > *count)
    {
        for (uint32 index = *count; index != 0; --index) values[index] = values[index - 1];
        ++*count;
    }
    return result;
}

bool inject(const char* operation) noexcept
{
    if (!failure_operation || strcmp(failure_operation, operation)) return false;
    if (failure_skip) { --failure_skip; return false; }
    failure_operation = nullptr;
    return true;
}

#define TEST_CREATE(name, parameters, arguments, counter) \
    VkResult VKAPI_CALL test_##name parameters \
    { \
        if (inject(#name)) return failure_result; \
        const VkResult result = name arguments; \
        if (result == VK_SUCCESS) ++live.counter; \
        return result; \
    }
#define TEST_DESTROY(name, parameters, arguments, handle, counter) \
    void VKAPI_CALL test_##name parameters \
    { \
        if (handle) --live.counter; \
        name arguments; \
    }
#define TEST_RESULT(name, parameters, arguments) \
    VkResult VKAPI_CALL test_##name parameters \
    { \
        return inject(#name) ? failure_result : name arguments; \
    }

VkResult VKAPI_CALL test_vkCreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator, VkInstance* instance)
{
    instance_layers = info->enabledLayerCount;
    instance_extension_count = info->enabledExtensionCount < 16 ? info->enabledExtensionCount : 16;
    for (uint32 index = 0; index < instance_extension_count; ++index)
        snprintf(instance_extension_names[index], VK_MAX_EXTENSION_NAME_SIZE, "%s", info->ppEnabledExtensionNames[index]);
    if (inject("vkCreateInstance")) return failure_result;
    const VkResult result = vkCreateInstance(info, allocator, instance);
    if (result == VK_SUCCESS) ++live.instances;
    return result;
}

TEST_CREATE(vkCreateDevice, (VkPhysicalDevice physical, const VkDeviceCreateInfo* info, const VkAllocationCallbacks* allocator, VkDevice* device),
    (physical, info, allocator, device), devices)
TEST_CREATE(vkCreateBuffer, (VkDevice device, const VkBufferCreateInfo* info, const VkAllocationCallbacks* allocator, VkBuffer* buffer),
    (device, info, allocator, buffer), buffers)
TEST_CREATE(vkAllocateMemory, (VkDevice device, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks* allocator, VkDeviceMemory* memory),
    (device, info, allocator, memory), memory)
TEST_CREATE(vkCreateSemaphore, (VkDevice device, const VkSemaphoreCreateInfo* info, const VkAllocationCallbacks* allocator, VkSemaphore* semaphore),
    (device, info, allocator, semaphore), semaphores)
TEST_CREATE(vkCreateImage, (VkDevice device, const VkImageCreateInfo* info, const VkAllocationCallbacks* allocator, VkImage* image),
    (device, info, allocator, image), images)
TEST_CREATE(vkCreateImageView, (VkDevice device, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* allocator, VkImageView* view),
    (device, info, allocator, view), views)
TEST_CREATE(vkCreateCommandPool, (VkDevice device, const VkCommandPoolCreateInfo* info, const VkAllocationCallbacks* allocator, VkCommandPool* pool),
    (device, info, allocator, pool), pools)
TEST_CREATE(vkCreateQueryPool, (VkDevice device, const VkQueryPoolCreateInfo* info, const VkAllocationCallbacks* allocator, VkQueryPool* pool),
    (device, info, allocator, pool), queries)
TEST_CREATE(vkCreateComputePipelines, (VkDevice device, VkPipelineCache cache, uint32 count, const VkComputePipelineCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkPipeline* pipelines), (device, cache, count, info, allocator, pipelines), pipelines)
TEST_CREATE(vkCreateGraphicsPipelines, (VkDevice device, VkPipelineCache cache, uint32 count, const VkGraphicsPipelineCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkPipeline* pipelines), (device, cache, count, info, allocator, pipelines), pipelines)
TEST_RESULT(vkBindBufferMemory, (VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset), (device, buffer, memory, offset))
TEST_RESULT(vkBindImageMemory, (VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize offset), (device, image, memory, offset))
TEST_RESULT(vkMapMemory, (VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void** data),
    (device, memory, offset, size, flags, data))
TEST_DESTROY(vkDestroyInstance, (VkInstance instance, const VkAllocationCallbacks* allocator), (instance, allocator), instance, instances)
TEST_DESTROY(vkDestroyDevice, (VkDevice device, const VkAllocationCallbacks* allocator), (device, allocator), device, devices)
TEST_DESTROY(vkDestroyBuffer, (VkDevice device, VkBuffer buffer, const VkAllocationCallbacks* allocator), (device, buffer, allocator), buffer, buffers)
TEST_DESTROY(vkFreeMemory, (VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* allocator), (device, memory, allocator), memory, memory)
TEST_DESTROY(vkDestroySemaphore, (VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks* allocator),
    (device, semaphore, allocator), semaphore, semaphores)
TEST_DESTROY(vkDestroyImage, (VkDevice device, VkImage image, const VkAllocationCallbacks* allocator), (device, image, allocator), image, images)
TEST_DESTROY(vkDestroyImageView, (VkDevice device, VkImageView view, const VkAllocationCallbacks* allocator), (device, view, allocator), view, views)
TEST_DESTROY(vkDestroyCommandPool, (VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* allocator), (device, pool, allocator), pool, pools)
TEST_DESTROY(vkDestroyQueryPool, (VkDevice device, VkQueryPool pool, const VkAllocationCallbacks* allocator), (device, pool, allocator), pool, queries)
TEST_DESTROY(vkDestroyPipeline, (VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks* allocator),
    (device, pipeline, allocator), pipeline, pipelines)

VkResult VKAPI_CALL test_vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* commands)
{
    if (inject("vkAllocateCommandBuffers")) return failure_result;
    const VkResult result = vkAllocateCommandBuffers(device, info, commands);
    if (result == VK_SUCCESS) command_allocations += info->commandBufferCount;
    return result;
}

void VKAPI_CALL test_vkFreeCommandBuffers(VkDevice device, VkCommandPool pool, uint32 count, const VkCommandBuffer* commands)
{
    command_frees += count;
    vkFreeCommandBuffers(device, pool, count, commands);
}

#undef TEST_CREATE
#undef TEST_DESTROY
#undef TEST_RESULT
}

// Inject only the test translation unit's native calls; normal calls still use the real driver.
#define vkCreateInstance test_vkCreateInstance
#define vkEnumerateDeviceExtensionProperties test_vkEnumerateDeviceExtensionProperties
#define vkEnumerateInstanceExtensionProperties test_vkEnumerateInstanceExtensionProperties
#define vkEnumeratePhysicalDevices test_vkEnumeratePhysicalDevices
#define vkCreateDevice test_vkCreateDevice
#define vkCreateBuffer test_vkCreateBuffer
#define vkAllocateMemory test_vkAllocateMemory
#define vkBindBufferMemory test_vkBindBufferMemory
#define vkMapMemory test_vkMapMemory
#define vkCreateSemaphore test_vkCreateSemaphore
#define vkCreateImage test_vkCreateImage
#define vkBindImageMemory test_vkBindImageMemory
#define vkCreateImageView test_vkCreateImageView
#define vkCreateCommandPool test_vkCreateCommandPool
#define vkAllocateCommandBuffers test_vkAllocateCommandBuffers
#define vkCreateQueryPool test_vkCreateQueryPool
#define vkCreateComputePipelines test_vkCreateComputePipelines
#define vkCreateGraphicsPipelines test_vkCreateGraphicsPipelines
#define vkDestroyInstance test_vkDestroyInstance
#define vkDestroyDevice test_vkDestroyDevice
#define vkDestroyBuffer test_vkDestroyBuffer
#define vkFreeMemory test_vkFreeMemory
#define vkDestroySemaphore test_vkDestroySemaphore
#define vkDestroyImage test_vkDestroyImage
#define vkDestroyImageView test_vkDestroyImageView
#define vkDestroyCommandPool test_vkDestroyCommandPool
#define vkFreeCommandBuffers test_vkFreeCommandBuffers
#define vkDestroyQueryPool test_vkDestroyQueryPool
#define vkDestroyPipeline test_vkDestroyPipeline
#include "../src/NoGraphicsAPI.cpp"

namespace
{
void check(bool condition, const char* message) noexcept
{
    if (condition) return;
    fprintf(stderr, "FAIL: %s [%s]\n", message, diagnostic);
    ++failures;
}

void receive_error(const char* message, bool fatal) noexcept
{
    ++reports;
    reported_fatal |= fatal;
    snprintf(diagnostic, sizeof(diagnostic), "%s", message);
}

void arm(const char* operation, VkResult result = VK_ERROR_OUT_OF_DEVICE_MEMORY, uint32 skip = 0) noexcept
{
    failure_operation = operation;
    failure_result = result;
    failure_skip = skip;
    reports = 0;
    reported_fatal = false;
    diagnostic[0] = '\0';
}

void unchanged(const NativeResources& before) noexcept
{
    check(live.instances == before.instances && live.devices == before.devices && live.buffers == before.buffers && live.memory == before.memory
        && live.semaphores == before.semaphores && live.images == before.images && live.views == before.views && live.pools == before.pools
        && live.queries == before.queries && live.pipelines == before.pipelines, "partial native resources are released");
}

void reported(const char* operation, const NativeResources& before, bool bytes = false) noexcept
{
    char result[64];
    snprintf(result, sizeof(result), "Vulkan result %d", int(failure_result));
    check(!failure_operation, "the selected native failure was reached");
    check(reports == 1 && !reported_fatal, "one nonfatal diagnostic accompanies the returned failure");
    check(strstr(diagnostic, operation) && strstr(diagnostic, result), "diagnostic identifies the failing native operation and result");
    if (bytes) check(strstr(diagnostic, "MiB requested") != nullptr, "allocation diagnostics include the requested memory size");
    unchanged(before);
}

void extension_failures(gpu::Device* device) noexcept
{
    const NativeResources before = live;
    const char* missing_heap[]{VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME, VK_EXT_MESH_SHADER_EXTENSION_NAME};
    advertised_device_extensions = missing_heap;
    arm(nullptr);
    gpu::DeviceInit init = gpu::create_device();
    check(!init.device && init.error == gpu::Error::unsupported, "missing required device extension rejects the device");
    check(reports == 1 && !reported_fatal && strstr(diagnostic, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME), "missing extension is reported once");
    check(!strstr(diagnostic, VK_EXT_MESH_SHADER_EXTENSION_NAME) && !strstr(diagnostic, VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME)
        && !strstr(diagnostic, VK_KHR_DEVICE_ADDRESS_COMMANDS_EXTENSION_NAME), "available and optional extensions are omitted from the diagnostic");
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device->physical_device, &properties);
    check(strstr(diagnostic, properties.deviceName) != nullptr, "missing device extensions identify the GPU");
    unchanged(before);

    const char* missing_heap_and_mesh[]{VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME};
    advertised_device_extensions = missing_heap_and_mesh;
    arm(nullptr);
    init = gpu::create_device();
    check(!init.device && init.error == gpu::Error::unsupported && reports == 1, "multiple missing extensions produce one failure diagnostic");
    check(strstr(diagnostic, VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME) && strstr(diagnostic, VK_EXT_MESH_SHADER_EXTENSION_NAME)
        && !strstr(diagnostic, VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME), "all missing required extensions are listed");
    unchanged(before);

    advertised_device_extensions = missing_heap;
    reject_first_device_only = true;
    device_extension_queries = 0;
    arm(nullptr);
    init = gpu::create_device();
    check(init.device && init.error == gpu::Error::none && device_extension_queries >= 2 && reports == 0,
        "a rejected GPU does not report startup errors when another compatible candidate is selected");
    if (init.device) { gpu::wait_idle(init.device); gpu::destroy_device(init.device); }
    reject_first_device_only = false;
    advertised_device_extensions = {};
    unchanged(before);

    hide_address_commands = true;
    arm(nullptr);
    init = gpu::create_device();
    check(init.device && init.error == gpu::Error::none && reports == 0,
        "missing optional device address commands succeeds without an extension diagnostic");
    if (init.device) { gpu::wait_idle(init.device); gpu::destroy_device(init.device); }
    hide_address_commands = false;
    unchanged(before);

#if defined(_WIN32)
    const char* missing_surface[]{VK_KHR_WIN32_SURFACE_EXTENSION_NAME, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME};
    advertised_instance_extensions = missing_surface;
    arm(nullptr);
    init = gpu::create_device({.window = reinterpret_cast<void*>(uintptr(1))});
    check(!init.device && init.error == gpu::Error::unsupported && reports == 1, "missing instance extensions fail before creating a window surface");
    check(strstr(diagnostic, VK_KHR_SURFACE_EXTENSION_NAME) && strstr(diagnostic, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME)
        && strstr(diagnostic, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME), "instance diagnostic includes missing required extensions and alternatives");
    check(!strstr(diagnostic, VK_KHR_WIN32_SURFACE_EXTENSION_NAME) && !strstr(diagnostic, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME)
        && !strstr(diagnostic, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME), "instance diagnostic omits available and unused optional extensions");
    advertised_instance_extensions = {};
    unchanged(before);
#endif
}

void heap_failures(gpu::Device* device) noexcept
{
    const char* operations[]{"vkCreateBuffer", "vkAllocateMemory", "vkBindBufferMemory", "vkMapMemory"};
    for (const char* operation : operations)
    {
        const NativeResources before = live;
        arm(operation, strcmp(operation, "vkMapMemory") == 0 ? VK_ERROR_MEMORY_MAP_FAILED : VK_ERROR_OUT_OF_DEVICE_MEMORY);
        const gpu::GpuHeap failed = gpu::create_gpu_heap(device, 1024 * 1024, gpu::MemoryType::cpu_visible);
        check(!failed.owner && !failed.range.cpu && !failed.range.gpu && !failed.range.size, "failed mapped heap returns an empty result");
        reported(operation, before, true);
        const gpu::GpuHeap retry = gpu::create_gpu_heap(device, 1024 * 1024, gpu::MemoryType::cpu_visible);
        check(retry.owner && retry.range.cpu && retry.range.gpu && retry.range.size == 1024 * 1024, "mapped heap creation succeeds after failure");
        gpu::destroy_gpu_heap(retry);
        unchanged(before);
    }
    const NativeResources before = live;
    arm("vkAllocateMemory");
    check(!gpu::create_texture_descriptor_heap(device, 4), "descriptor heap propagates backing allocation failure");
    reported("vkAllocateMemory", before, true);
    gpu::TextureDescriptorHeap* textures = gpu::create_texture_descriptor_heap(device, 4);
    check(textures != nullptr, "texture descriptor heap creation recovers");
    gpu::destroy_texture_descriptor_heap(textures);
    arm("vkMapMemory", VK_ERROR_MEMORY_MAP_FAILED);
    check(!gpu::create_sampler_descriptor_heap(device, 4), "sampler heap propagates backing map failure");
    reported("vkMapMemory", before, true);
    gpu::SamplerDescriptorHeap* samplers = gpu::create_sampler_descriptor_heap(device, 4);
    check(samplers != nullptr, "sampler descriptor heap creation recovers");
    gpu::destroy_sampler_descriptor_heap(samplers);
    unchanged(before);
}

void texture_failures(gpu::Device* device) noexcept
{
    const gpu::TextureDesc desc{.extent = {.x = 16, .y = 16, .z = 1}, .usage = gpu::TextureUsage::sampled | gpu::TextureUsage::color_attachment};
    const gpu::SizeAlign size = gpu::get_texture_size_align(device, desc);
    check(size.size != 0, "ordinary texture requirements are available");
    if (!size.size) return;
    const NativeResources initial = live;
    arm("vkAllocateMemory");
    const gpu::TextureHeap failed = gpu::create_texture_heap(device, size.size);
    check(!failed.owner && !failed.size, "texture heap allocation failure returns an empty result");
    reported("vkAllocateMemory", initial, true);
    const gpu::TextureHeap heap = gpu::create_texture_heap(device, size.size);
    check(heap.owner != nullptr, "texture heap creation recovers");
    if (heap.owner)
    {
        const NativeResources before = live;
        const char* operations[]{"vkCreateImage", "vkBindImageMemory"};
        for (const char* operation : operations)
        {
            arm(operation);
            check(!gpu::create_texture(device, desc, heap, 0), "image creation or binding failure returns null");
            reported(operation, before);
            gpu::Texture* retry = gpu::create_texture(device, desc, heap, 0);
            check(retry != nullptr, "image creation succeeds after failure");
            gpu::destroy_texture(retry);
            unchanged(before);
        }
        gpu::Texture* texture = gpu::create_texture(device, desc, heap, 0);
        check(texture != nullptr, "render view fixture texture is available");
        if (texture)
        {
            const NativeResources before_view = live;
            arm("vkCreateImageView");
            check(!gpu::create_render_view(texture), "render view creation failure returns null");
            reported("vkCreateImageView", before_view);
            gpu::RenderView* view = gpu::create_render_view(texture);
            check(view != nullptr, "render view creation succeeds after failure");
            gpu::destroy_render_view(view);
            gpu::destroy_texture(texture);
        }
    }
    gpu::destroy_texture_heap(heap);
    unchanged(initial);
}

void utility_failures(gpu::Device* device) noexcept
{
    const NativeResources initial = live;
    const char* upload_operations[]{"vkCreateBuffer", "vkAllocateMemory", "vkMapMemory", "vkCreateSemaphore", "vkCreateCommandPool",
        "vkAllocateCommandBuffers", "vkCreateQueryPool"};
    for (uint32 index = 0; index < sizeof(upload_operations) / sizeof(upload_operations[0]); ++index)
    {
        arm(upload_operations[index], index == 2 ? VK_ERROR_MEMORY_MAP_FAILED : VK_ERROR_OUT_OF_DEVICE_MEMORY, index >= 4 ? 1 : 0);
        gpu::UploadQueue failed(device, 65536);
        check(!failed.valid() && !failed.stats().capacity && !failed.stats().pending_batches, "failed uploader constructor leaves an empty invalid queue");
        reported(upload_operations[index], initial, index <= 2);
        failed.destroy();
        unchanged(initial);
        gpu::UploadQueue retry(device, 65536);
        check(retry.valid() && retry.stats().capacity == 65536, "uploader construction succeeds after native failure");
        gpu::UploadQueue moved(static_cast<gpu::UploadQueue&&>(retry));
        check(moved.valid() && !retry.valid(), "uploader validity follows ownership after a move");
        moved.destroy();
        check(!moved.valid(), "destroying the uploader clears validity");
        unchanged(initial);
    }

    const gpu::TextureDesc desc{.extent = {.x = 16, .y = 16, .z = 1}};
    const gpu::SizeAlign size = gpu::get_texture_size_align(device, desc);
    const uint64 alignment = gpu::get_device_caps(device).texture_heap_alignment;
    const gpu::TextureHeap heap = gpu::create_texture_heap(device, (size.size + alignment - 1) & ~(alignment - 1));
    check(heap.owner != nullptr, "texture allocator failure fixture is available");
    if (heap.owner)
    {
        gpu::TextureAllocator allocator(device, heap, 1);
        const char* operations[]{"vkCreateImage", "vkBindImageMemory"};
        for (const char* operation : operations)
        {
            const NativeResources before = live;
            arm(operation);
            gpu::PlacedTexture failed = allocator.allocate(desc);
            check(!failed.texture && !failed.token, "failed texture creation returns an empty allocator result");
            reported(operation, before);
            gpu::PlacedTexture retry = allocator.allocate(desc);
            check(retry.texture != nullptr, "failed texture reservation is reusable in a one-slot heap");
            check(!allocator.allocate(desc).texture, "successful retry consumes the only texture reservation");
            allocator.free(retry);
            check(!retry.texture && !retry.token, "freeing the successful retry restores an empty allocation");
            unchanged(before);
        }
    }
    gpu::destroy_texture_heap(heap);
    unchanged(initial);
}

void command_failures(gpu::Device* device) noexcept
{
    const NativeResources before = live;
    arm("vkCreateSemaphore");
    check(!gpu::create_timeline_semaphore(device), "timeline creation failure returns null");
    reported("vkCreateSemaphore", before);
    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(device);
    check(timeline != nullptr, "timeline creation succeeds after failure");
    gpu::destroy_timeline_semaphore(timeline);
    arm("vkCreateCommandPool");
    check(!gpu::create_command_pool(device), "command pool creation failure returns null");
    reported("vkCreateCommandPool", before);
    gpu::CommandPool* pool = gpu::create_command_pool(device);
    check(pool != nullptr, "command pool creation succeeds after failure");
    gpu::destroy_command_pool(pool);
    const char* operations[]{"vkAllocateCommandBuffers", "vkCreateQueryPool"};
    for (const char* operation : operations)
    {
        pool = gpu::create_command_pool(device);
        check(pool != nullptr, "command failure fixture pool is available");
        if (!pool) continue;
        const NativeResources before_commands = live;
        const uint32 allocations = command_allocations, frees = command_frees;
        arm(operation);
        check(!gpu::begin_commands(pool), "first command recording failure returns null");
        reported(operation, before_commands);
        check(command_allocations - allocations == command_frees - frees, "failed first command context frees its native command buffer");
        gpu::CommandBuffer* commands = gpu::begin_commands(pool);
        check(commands != nullptr, "first command recording succeeds after failure");
        if (commands) gpu::end_commands(commands);
        gpu::destroy_command_pool(pool);
        unchanged(before);
    }
}

gpu::ByteSpan read_shader(const char* path) noexcept
{
    FILE* file = fopen(path, "rb");
    check(file != nullptr, "compiled pipeline fixture is available");
    if (!file) return {};
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    rewind(file);
    byte* data = static_cast<byte*>(malloc(size_t(size)));
    const bool valid = size > 0 && fread(data, 1, size_t(size), file) == size_t(size);
    fclose(file);
    check(valid, "compiled pipeline fixture loads completely");
    if (!valid) { free(data); return {}; }
    return {data, uint64(size)};
}

void pipeline_failures(gpu::Device* device) noexcept
{
    const gpu::ByteSpan compute = read_shader(NOGRAPHICSAPI_FAILURE_COMPUTE);
    const gpu::ByteSpan vertex = read_shader(NOGRAPHICSAPI_FAILURE_VERTEX);
    const gpu::ByteSpan fragment = read_shader(NOGRAPHICSAPI_FAILURE_FRAGMENT);
    const NativeResources before = live;
    if (compute.data)
    {
        const gpu::ShaderStage stage{.code = compute, .entry_point = "computeMain"};
        arm("vkCreateComputePipelines");
        check(!gpu::create_compute_pso(device, stage), "compute pipeline failure returns null");
        reported("vkCreateComputePipelines", before);
        gpu::PSO* pso = gpu::create_compute_pso(device, stage);
        check(pso != nullptr, "compute pipeline creation succeeds after failure");
        gpu::destroy_pso(pso);
    }
    if (vertex.data && fragment.data)
    {
        const gpu::ColorTargetDesc target{.format = gpu::Format::rgba8_unorm};
        const gpu::GraphicsPSODesc desc{.vertex = {.code = vertex, .entry_point = "vertexMain"},
            .fragment = {.code = fragment, .entry_point = "fragmentMain"}, .color_targets = {&target, 1}};
        arm("vkCreateGraphicsPipelines");
        check(!gpu::create_graphics_pso(device, desc), "graphics pipeline failure returns null");
        reported("vkCreateGraphicsPipelines", before);
        gpu::PSO* pso = gpu::create_graphics_pso(device, desc);
        check(pso != nullptr, "graphics pipeline creation succeeds after failure");
        gpu::destroy_pso(pso);
    }
    free(const_cast<byte*>(compute.data));
    free(const_cast<byte*>(vertex.data));
    free(const_cast<byte*>(fragment.data));
    unchanged(before);
}

const char* const headless_extensions[]{VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
#if defined(NOGRAPHICSAPI_TEST_WAYLAND)
const char* const wayland_extensions[]{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME};
#endif
#if defined(NOGRAPHICSAPI_TEST_XCB)
const char* const xcb_extensions[]{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XCB_SURFACE_EXTENSION_NAME};
#endif

// Headless and Wayland surfaces have no fixed extent and ask drawable_extent. An X11 window has a fixed extent.
struct TestWindow
{
    gpu::uint32x2 extent;
    uint32 surface_calls, extent_calls;
#if defined(NOGRAPHICSAPI_TEST_WAYLAND)
    wl_display* display;
    wl_compositor* compositor;
    wl_surface* surface;
#endif
#if defined(NOGRAPHICSAPI_TEST_XCB)
    xcb_connection_t* connection;
    xcb_window_t window;
#endif
};

bool instance_extension_available(const char* name) noexcept
{
    VkExtensionProperties extensions[gpu::max_instance_extensions]{};
    uint32 count = gpu::max_instance_extensions;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions);
    return gpu::has_name({extensions, count}, name);
}

VkSurfaceKHR_T* create_headless_surface(VkInstance_T* instance, void* user) noexcept
{
    const PFN_vkCreateHeadlessSurfaceEXT create = gpu::load_instance_proc<PFN_vkCreateHeadlessSurfaceEXT>(instance, "vkCreateHeadlessSurfaceEXT");
    const VkHeadlessSurfaceCreateInfoEXT info{.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    ++static_cast<TestWindow*>(user)->surface_calls;
    return create && create(instance, &info, nullptr, &surface) == VK_SUCCESS ? surface : VK_NULL_HANDLE;
}

#if defined(NOGRAPHICSAPI_TEST_WAYLAND)
void bind_compositor(void* user, wl_registry* registry, uint32 name, const char* interface, uint32) noexcept
{
    if (!strcmp(interface, wl_compositor_interface.name))
        static_cast<TestWindow*>(user)->compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 1));
}

void remove_global(void*, wl_registry*, uint32) noexcept
{
}

const wl_registry_listener registry_listener{.global = bind_compositor, .global_remove = remove_global};

VkSurfaceKHR_T* create_wayland_surface(VkInstance_T* instance, void* user) noexcept
{
    TestWindow& window = *static_cast<TestWindow*>(user);
    const PFN_vkCreateWaylandSurfaceKHR create = gpu::load_instance_proc<PFN_vkCreateWaylandSurfaceKHR>(instance, "vkCreateWaylandSurfaceKHR");
    const VkWaylandSurfaceCreateInfoKHR info{.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR, .display = window.display, .surface = window.surface};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    ++window.surface_calls;
    return create && create(instance, &info, nullptr, &surface) == VK_SUCCESS ? surface : VK_NULL_HANDLE;
}
#endif

#if defined(NOGRAPHICSAPI_TEST_XCB)
VkSurfaceKHR_T* create_xcb_surface(VkInstance_T* instance, void* user) noexcept
{
    TestWindow& window = *static_cast<TestWindow*>(user);
    const PFN_vkCreateXcbSurfaceKHR create = gpu::load_instance_proc<PFN_vkCreateXcbSurfaceKHR>(instance, "vkCreateXcbSurfaceKHR");
    const VkXcbSurfaceCreateInfoKHR info{.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR, .connection = window.connection, .window = window.window};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    ++window.surface_calls;
    return create && create(instance, &info, nullptr, &surface) == VK_SUCCESS ? surface : VK_NULL_HANDLE;
}
#endif

VkSurfaceKHR_T* create_no_surface(VkInstance_T*, void*) noexcept
{
    return VK_NULL_HANDLE;
}

gpu::uint32x2 test_window_extent(void* user) noexcept
{
    TestWindow& window = *static_cast<TestWindow*>(user);
    ++window.extent_calls;
    return window.extent;
}

void wsi_failures() noexcept
{
    const NativeResources before = live;
    arm(nullptr);
    const gpu::DeviceInit init = gpu::create_device({.create_surface = create_no_surface});
    check(!init.device && init.error == gpu::Error::unsupported && reports == 1 && !reported_fatal && strstr(diagnostic, "returned no Vulkan surface"),
        "a create_surface callback without a surface reports why presentation is unsupported");
    unchanged(before);
    // Names the device enables on its own may repeat in instance_extensions. Each name reaches vkCreateInstance once.
    const char* repeated[3]{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME};
    uint32 repeated_count = 2;
    if (instance_extension_available(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) repeated[repeated_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    arm(nullptr);
    const gpu::DeviceInit repeated_init =
        gpu::create_device({.instance_extensions = repeated, .instance_extension_count = repeated_count, .create_surface = create_no_surface});
    check(!repeated_init.device && reports == 1 && strstr(diagnostic, "returned no Vulkan surface"), "instance creation succeeds with repeated instance_extensions");
    for (uint32 name = 0; name < repeated_count; ++name)
    {
        uint32 occurrences = 0;
        for (uint32 index = 0; index < instance_extension_count; ++index)
            if (strcmp(instance_extension_names[index], repeated[name]) == 0) ++occurrences;
        check(occurrences == 1, "a repeated instance extension name reaches vkCreateInstance once");
    }
    unchanged(before);
}

void validation_disabled() noexcept
{
    const NativeResources before = live;
    arm(nullptr);
    const gpu::DeviceInit init = gpu::create_device({.validation = false});
    check(init.device != nullptr, "device creation without validation succeeds");
    if (!init.device) return;
    check(!instance_layers && !init.device->debug_messenger, "validation = false enables no layer and no messenger");
    gpu::destroy_device(init.device);
    unchanged(before);
}

// Returns false when no GPU presents to the surface.
bool present_frames(const char* name, const char* const* extensions, VkSurfaceKHR_T* (*create_surface)(VkInstance_T*, void*), TestWindow& window,
    bool fixed_extent) noexcept
{
    const uint32 earlier_failures = failures;
    window.surface_calls = window.extent_calls = 0;
    arm(nullptr);
    const gpu::DeviceInit init = gpu::create_device({.swapchain_format = gpu::Format::bgra8_srgb, .instance_extensions = extensions,
        .instance_extension_count = 2, .create_surface = create_surface, .drawable_extent = test_window_extent, .wsi_user = &window});
    if (init.error == gpu::Error::unsupported && !strstr(diagnostic, "create_surface"))
    {
        printf("No GPU presents to a %s surface.\n", name);
        return false;
    }
    check(init.device && window.surface_calls == 1, "device creation calls create_surface once with wsi_user and succeeds");
    if (!init.device) return true;
    gpu::Device* device = init.device;
    gpu::CommandPool* pool = gpu::create_command_pool(device);
    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(device);
    check(pool && timeline, "presentation fixture is available");
    for (uint64 frame = 1; pool && timeline && frame <= 4; ++frame)
    {
        // A surface without a fixed extent reports no resize, so acquire alone must follow drawable_extent.
        if (frame == 3 && !fixed_extent) window.extent = {.x = 80, .y = 56};
        gpu::reset_command_pool(pool);
        gpu::CommandBuffer* commands = gpu::begin_commands(pool);
        const gpu::SwapchainFrame acquired = gpu::acquire(commands);
        check(acquired.render_view && acquired.extent.x == window.extent.x && acquired.extent.y == window.extent.y,
            "acquire returns an image of the window extent");
        gpu::end_commands(commands);
        if (acquired.render_view)
            gpu::submit_and_present(device, {.commands = {&commands, 1}, .completion = {.semaphore = timeline, .value = frame}});
        else
            gpu::submit(device, {.commands = {&commands, 1}, .completion = {.semaphore = timeline, .value = frame}});
        gpu::wait_timeline({.semaphore = timeline, .value = frame});
    }
    const gpu::uint32x2 extent = gpu::get_drawable_extent(device);
    check(extent.x == window.extent.x && extent.y == window.extent.y, "get_drawable_extent returns the window extent");
    check(fixed_extent ? window.extent_calls == 0 : window.extent_calls != 0, "drawable_extent is asked only when the surface has no fixed extent");
    check(reports == 0, "presentation reports no diagnostic");
    check(gpu::get_device_caps(device).swapchain_maintenance1, "caps report swapchain_maintenance1 on a presentation device");
    gpu::wait_idle(device);
    gpu::destroy_timeline_semaphore(timeline);
    gpu::destroy_command_pool(pool);
    gpu::destroy_device(device);
    printf("Linux presentation, %s surface: %u failures\n", name, failures - earlier_failures);
    return true;
}

// Every surface that a GPU presents to runs. Headless and Wayland surfaces have no fixed extent, and an X11 window has one.
int present_case() noexcept
{
    gpu::set_error_callback(receive_error);
    const gpu::uint32x2 initial_extent{.x = 64, .y = 48};
    TestWindow window{.extent = initial_extent};
    bool presented = false;
    if (instance_extension_available(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME))
        presented = present_frames("headless", headless_extensions, create_headless_surface, window, false);
#if defined(NOGRAPHICSAPI_TEST_WAYLAND)
    window.display = instance_extension_available(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME) ? wl_display_connect(nullptr) : nullptr;
    if (window.display)
    {
        wl_registry* registry = wl_display_get_registry(window.display);
        wl_registry_add_listener(registry, &registry_listener, &window);
        wl_display_roundtrip(window.display);
        if (window.compositor)
        {
            window.extent = initial_extent;
            window.surface = wl_compositor_create_surface(window.compositor);
            presented = present_frames("Wayland", wayland_extensions, create_wayland_surface, window, false) || presented;
            wl_surface_destroy(window.surface);
            wl_compositor_destroy(window.compositor);
        }
        wl_registry_destroy(registry);
        wl_display_disconnect(window.display);
    }
#endif
#if defined(NOGRAPHICSAPI_TEST_XCB)
    window.connection = instance_extension_available(VK_KHR_XCB_SURFACE_EXTENSION_NAME) ? xcb_connect(nullptr, nullptr) : nullptr;
    if (window.connection && !xcb_connection_has_error(window.connection))
    {
        window.extent = initial_extent;
        const xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(window.connection)).data;
        window.window = xcb_generate_id(window.connection);
        xcb_create_window(window.connection, XCB_COPY_FROM_PARENT, window.window, screen->root, 0, 0, uint16(window.extent.x), uint16(window.extent.y), 0,
            XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
        xcb_flush(window.connection);
        presented = present_frames("X11", xcb_extensions, create_xcb_surface, window, true) || presented;
        xcb_destroy_window(window.connection, window.window);
    }
    if (window.connection) xcb_disconnect(window.connection);
#endif
    gpu::set_error_callback(nullptr);
    unchanged({});
    if (!presented)
    {
        printf("SKIP: no GPU presents to a headless, Wayland or X11 surface.\n");
        return 77;
    }
    if (hide_khr_swapchain_maintenance1) check(hidden_extensions != 0, "the KHR swapchain_maintenance1 name is hidden from the device");
    return failures ? 1 : 0;
}

int validation_forwarding_case() noexcept
{
#if defined(NDEBUG)
    printf("SKIP: builds with NDEBUG have no validation.\n");
    return 77;
#else
    gpu::set_error_callback(receive_error);
    const gpu::DeviceInit init = gpu::create_device();
    if (init.error == gpu::Error::unsupported)
    {
        printf("SKIP: required Vulkan device features are unavailable.\n");
        return 77;
    }
    if (!init.device) return 1;
    gpu::Device* device = init.device;
    if (!device->debug_messenger)
    {
        printf("SKIP: VK_EXT_debug_utils is unavailable.\n");
        gpu::destroy_device(device);
        return 77;
    }
    const PFN_vkSubmitDebugUtilsMessageEXT submit_message =
        gpu::load_instance_proc<PFN_vkSubmitDebugUtilsMessageEXT>(device->instance, "vkSubmitDebugUtilsMessageEXT");
    check(submit_message != nullptr, "vkSubmitDebugUtilsMessageEXT is available with VK_EXT_debug_utils");
    if (submit_message)
    {
        const VkDebugUtilsMessengerCallbackDataEXT info{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT, .pMessage = "info probe"};
        const VkDebugUtilsMessengerCallbackDataEXT warning{.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT, .pMessage = "warning probe"};
        arm(nullptr);
        submit_message(device->instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &info);
        check(reports == 0, "messages below warning severity are not forwarded");
        submit_message(device->instance, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &warning);
        check(reports == 1 && !reported_fatal && !strcmp(diagnostic, "NoGraphicsAPI validation: warning probe"),
            "a validation warning reaches the error callback as nonfatal");
    }
    gpu::destroy_device(device);
    gpu::set_error_callback(nullptr);
    printf("Validation forwarding: %u failures\n", failures);
    return failures ? 1 : 0;
#endif
}

#if defined(_WIN32)
DWORD WINAPI callback_thread(void*) noexcept
{
    arm("vkCreateInstance", VK_ERROR_OUT_OF_HOST_MEMORY);
    const gpu::DeviceInit init = gpu::create_device();
    return !init.device && init.error == gpu::Error::out_of_memory && !failure_operation ? 0 : 1;
}
#endif
}

int main(int argc, char** argv)
{
    if (argc == 2 && !strcmp(argv[1], "--present")) return present_case();
    if (argc == 2 && !strcmp(argv[1], "--present-ext-swapchain-maintenance1"))
    {
        hide_khr_swapchain_maintenance1 = true;
        return present_case();
    }
    if (argc == 2 && !strcmp(argv[1], "--validation-forwarding")) return validation_forwarding_case();
    gpu::set_error_callback(receive_error);
    arm("vkCreateInstance", VK_ERROR_OUT_OF_HOST_MEMORY);
    gpu::DeviceInit init = gpu::create_device();
    check(!init.device && init.error == gpu::Error::out_of_memory, "instance allocation failure returns an error instead of terminating");
    reported("vkCreateInstance", {});
#if !defined(_WIN32)
    int window = 0;
    arm(nullptr);
    init = gpu::create_device({.window = &window});
    check(!init.device && init.error == gpu::Error::unsupported && reports == 1 && !reported_fatal && strstr(diagnostic, "create_surface"),
        "presentation without a create_surface callback reports why it is unsupported");
#endif
    wsi_failures();
    validation_disabled();
    arm(nullptr);
    init = gpu::create_device({.timestamp_query_count = 8});
    if (init.error == gpu::Error::unsupported)
    {
        printf("SKIP: required Vulkan device features are unavailable.\n");
        return 77;
    }
    check(init.device && init.error == gpu::Error::none, "device creation succeeds after instance failure");
    if (!init.device) return 1;
    gpu::Device* device = init.device;
    extension_failures(device);
    const NativeResources before_device = live;
    arm("vkCreateDevice");
    init = gpu::create_device();
    check(!init.device && init.error == gpu::Error::out_of_memory, "logical device allocation failure returns an error");
    reported("vkCreateDevice", before_device);
    heap_failures(device);
    texture_failures(device);
    command_failures(device);
    utility_failures(device);
    pipeline_failures(device);
    arm("vkCreateBuffer");
    gpu::set_error_callback(nullptr);
    const gpu::GpuHeap silent = gpu::create_gpu_heap(device, 1024);
    check(!silent.owner && !failure_operation && reports == 0, "clearing the callback suppresses diagnostics without changing failure returns");
    gpu::set_error_callback(receive_error);
#if defined(_WIN32)
    arm(nullptr);
    HANDLE worker = CreateThread(nullptr, 0, callback_thread, nullptr, 0, nullptr);
    check(worker != nullptr, "callback isolation worker starts");
    if (worker)
    {
        WaitForSingleObject(worker, INFINITE);
        DWORD result = 1;
        GetExitCodeThread(worker, &result);
        CloseHandle(worker);
        check(result == 0 && reports == 0, "startup callback is not inherited by another thread");
    }
    arm("vkCreateBuffer");
    check(!gpu::create_gpu_heap(device, 1024).owner, "main thread callback remains installed after the worker finishes");
    reported("vkCreateBuffer", before_device, true);
#endif
    gpu::wait_idle(device);
    gpu::destroy_device(device);
    gpu::set_error_callback(nullptr);
    unchanged({});
    printf("Startup failure returns and cleanup: %u failures\n", failures);
    return failures ? 1 : 0;
}
