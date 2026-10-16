#include <NoGraphicsAPI/NoGraphicsAPI.hpp>
#include <NoGraphicsAPIUtility/texture_allocator.hpp>
#include <NoGraphicsAPIUtility/upload_queue.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#endif
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(NOGRAPHICSAPI_TEST_XCB)
#include <unistd.h>
#endif

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
uint32 command_allocations = 0, command_frees = 0;
uint32 failures = 0, reports = 0;
bool reported_fatal = false;
char diagnostic[1024]{};
uint32 instance_layers = 0;
bool instance_debug_utils = false;

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
    instance_debug_utils = false;
    for (uint32 index = 0; index < info->enabledExtensionCount; ++index)
        instance_debug_utils |= !strcmp(info->ppEnabledExtensionNames[index], VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
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
TEST_RESULT(vkWaitForFences, (VkDevice device, uint32 count, const VkFence* fences, VkBool32 wait_all, uint64 timeout),
    (device, count, fences, wait_all, timeout))
TEST_RESULT(vkGetFenceStatus, (VkDevice device, VkFence fence), (device, fence))
TEST_RESULT(vkMapMemory, (VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size, VkMemoryMapFlags flags, void** data),
    (device, memory, offset, size, flags, data))
TEST_RESULT(vkQueueSubmit2, (VkQueue queue, uint32 count, const VkSubmitInfo2* submits, VkFence fence), (queue, count, submits, fence))
TEST_RESULT(vkWaitSemaphores, (VkDevice device, const VkSemaphoreWaitInfo* info, uint64 timeout), (device, info, timeout))
TEST_RESULT(vkGetQueryPoolResults, (VkDevice device, VkQueryPool pool, uint32 first, uint32 count, size_t size, void* data, VkDeviceSize stride,
    VkQueryResultFlags flags), (device, pool, first, count, size, data, stride, flags))
TEST_RESULT(vkDeviceWaitIdle, (VkDevice device), (device))
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
#define vkCreateDevice test_vkCreateDevice
#define vkCreateBuffer test_vkCreateBuffer
#define vkAllocateMemory test_vkAllocateMemory
#define vkBindBufferMemory test_vkBindBufferMemory
#define vkMapMemory test_vkMapMemory
#define vkWaitForFences test_vkWaitForFences
#define vkGetFenceStatus test_vkGetFenceStatus
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
#define vkQueueSubmit2 test_vkQueueSubmit2
#define vkWaitSemaphores test_vkWaitSemaphores
#define vkGetQueryPoolResults test_vkGetQueryPoolResults
#define vkDeviceWaitIdle test_vkDeviceWaitIdle
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
    gpu::CommandPool* pool = gpu::create_command_pool(device);
    gpu::CommandBuffer* commands = pool ? gpu::begin_commands(pool) : nullptr;
    check(heap.owner && commands, "texture creation resources recover");
    if (heap.owner && commands)
    {
        const NativeResources before = live;
        const char* operations[]{"vkCreateImage", "vkBindImageMemory"};
        for (const char* operation : operations)
        {
            arm(operation);
            check(!gpu::create_texture(commands, desc, heap, 0), "image creation or binding failure returns null");
            reported(operation, before);
            gpu::Texture* retry = gpu::create_texture(commands, desc, heap, 0);
            check(retry != nullptr, "image creation succeeds after failure");
            gpu::end_commands(commands);
            gpu::reset_command_pool(pool);
            gpu::destroy_texture(retry);
            commands = gpu::begin_commands(pool);
            unchanged(before);
        }
        gpu::Texture* texture = gpu::create_texture(commands, desc, heap, 0);
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
            gpu::end_commands(commands);
            gpu::reset_command_pool(pool);
            gpu::destroy_texture(texture);
            commands = gpu::begin_commands(pool);
        }
        gpu::end_commands(commands);
    }
    gpu::destroy_command_pool(pool);
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
    gpu::CommandPool* pool = gpu::create_command_pool(device);
    check(heap.owner && pool, "texture allocator failure fixture is available");
    if (heap.owner && pool)
    {
        gpu::TextureAllocator allocator(device, heap, 1);
        const char* operations[]{"vkCreateImage", "vkBindImageMemory"};
        for (const char* operation : operations)
        {
            gpu::CommandBuffer* commands = gpu::begin_commands(pool);
            check(commands != nullptr, "texture allocator initialization commands are available");
            if (!commands) break;
            const NativeResources before = live;
            arm(operation);
            gpu::PlacedTexture failed = allocator.allocate(commands, desc);
            check(!failed.texture && !failed.token, "failed texture creation returns an empty allocator result");
            reported(operation, before);
            gpu::PlacedTexture retry = allocator.allocate(commands, desc);
            check(retry.texture != nullptr, "failed texture reservation is reusable in a one-slot heap");
            check(!allocator.allocate(commands, desc).texture, "successful retry consumes the only texture reservation");
            gpu::end_commands(commands);
            gpu::reset_command_pool(pool);
            allocator.free(retry);
            check(!retry.texture && !retry.token, "freeing the successful retry restores an empty allocation");
            unchanged(before);
        }
    }
    gpu::destroy_command_pool(pool);
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

const char* lost_operation = nullptr;

void receive_device_lost(const char* message, bool fatal) noexcept
{
    if (!fatal) return;
    if (strstr(message, lost_operation) && strstr(message, "VK_ERROR_DEVICE_LOST"))
    {
        printf("PASS: %s reports a lost device as fatal\n", lost_operation);
        fflush(stdout);
        _Exit(0);
    }
    fprintf(stderr, "FAIL: unexpected fatal report: %s\n", message);
    fflush(stderr);
    _Exit(1);
}

void arm_lost(const char* operation, const char* site) noexcept
{
    if (!strcmp(operation, site)) arm(site, VK_ERROR_DEVICE_LOST);
}

bool lost_without_stop(const char* operation, const char* site) noexcept
{
    if (strcmp(operation, site)) return false;
    if (failure_operation)
        fprintf(stderr, "FAIL: %s was never called\n", site);
    else
        fprintf(stderr, "FAIL: %s returned after a lost device without a fatal report\n", site);
    return true;
}

int present_case() noexcept;

int device_lost_case(const char* operation) noexcept
{
    lost_operation = operation;
    if (!strcmp(operation, "vkWaitForFences") || !strcmp(operation, "vkGetFenceStatus")) return present_case();
    const gpu::DeviceInit init = gpu::create_device({.timestamp_query_count = 8});
    if (init.error == gpu::Error::unsupported)
    {
        printf("SKIP: required Vulkan device features are unavailable.\n");
        return 77;
    }
    if (!init.device)
    {
        fprintf(stderr, "FAIL: device creation failed\n");
        return 1;
    }
    gpu::Device* device = init.device;
    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(device);
    gpu::CommandPool* pool = gpu::create_command_pool(device);
    gpu::CommandBuffer* commands = timeline && pool ? gpu::begin_commands(pool) : nullptr;
    if (!commands)
    {
        fprintf(stderr, "FAIL: device-lost fixture is unavailable\n");
        return 1;
    }
    uint64 timestamp = 0;
    gpu::write_timestamp(commands, &timestamp);
    if (commands->timestamp_count == 0 && !strcmp(operation, "vkGetQueryPoolResults"))
    {
        printf("SKIP: the queue has no 64-bit timestamps.\n");
        return 77;
    }
    gpu::end_commands(commands);
    gpu::set_error_callback(receive_device_lost);
    arm_lost(operation, "vkQueueSubmit2");
    gpu::submit(device, {.commands = {&commands, 1}, .completion = {.semaphore = timeline, .value = 1}});
    if (lost_without_stop(operation, "vkQueueSubmit2")) return 1;
    arm_lost(operation, "vkWaitSemaphores");
    gpu::wait_timeline({.semaphore = timeline, .value = 1});
    if (lost_without_stop(operation, "vkWaitSemaphores")) return 1;
    arm_lost(operation, "vkGetQueryPoolResults");
    gpu::read_timestamps(pool);
    if (lost_without_stop(operation, "vkGetQueryPoolResults")) return 1;
    arm_lost(operation, "vkDeviceWaitIdle");
    gpu::wait_idle(device);
    if (lost_without_stop(operation, "vkDeviceWaitIdle")) return 1;
    fprintf(stderr, "FAIL: unknown device-lost operation %s\n", operation);
    return 1;
}

const char* const headless_extensions[]{VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
#if defined(NOGRAPHICSAPI_TEST_XCB)
const char* const xcb_extensions[]{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_XCB_SURFACE_EXTENSION_NAME};
#endif

// A headless surface has no fixed extent and asks drawable_extent. An X11 window has a fixed extent.
struct TestWindow
{
    gpu::uint32x2 extent;
    uint32 surface_calls, extent_calls;
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
}

void validation_disabled() noexcept
{
    const NativeResources before = live;
    arm(nullptr);
    const gpu::DeviceInit init = gpu::create_device({.validation = false});
    check(init.device != nullptr, "device creation without validation succeeds");
    if (!init.device) return;
    check(!instance_layers && !instance_debug_utils && !init.device->debug_messenger, "validation = false enables no layer and no messenger");
    gpu::destroy_device(init.device);
    unchanged(before);
}

// get_drawable_extent marks the swapchain for recreation once it sees the new extent.
bool resize(gpu::Device* device, TestWindow& window, gpu::uint32x2 extent) noexcept
{
    window.extent = extent;
#if defined(NOGRAPHICSAPI_TEST_XCB)
    if (window.connection)
    {
        const uint32 values[]{extent.x, extent.y};
        xcb_configure_window(window.connection, window.window, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
        xcb_flush(window.connection);
        // The driver sees an X11 resize some time after the server applies it.
        for (uint32 attempt = 0; attempt < 10000; ++attempt)
        {
            const gpu::uint32x2 current = gpu::get_drawable_extent(device);
            if (current.x == extent.x && current.y == extent.y) return true;
            usleep(100);
        }
        return false;
    }
#endif
    const gpu::uint32x2 current = gpu::get_drawable_extent(device);
    return current.x == extent.x && current.y == extent.y;
}

// A present fence stays pending until swapchain recreation polls it or wait_idle waits for it.
int lose_present_fence(gpu::Device* device, gpu::CommandPool* pool, TestWindow& window) noexcept
{
    gpu::set_error_callback(receive_device_lost);
    arm_lost(lost_operation, "vkGetFenceStatus");
    if (!strcmp(lost_operation, "vkGetFenceStatus"))
    {
        if (!resize(device, window, {.x = window.extent.x + 16, .y = window.extent.y + 16}))
        {
            fprintf(stderr, "FAIL: the drawable extent did not follow the window resize\n");
            return 1;
        }
        gpu::reset_command_pool(pool);
        (void)gpu::acquire(gpu::begin_commands(pool));
    }
    if (lost_without_stop(lost_operation, "vkGetFenceStatus")) return 1;
    arm_lost(lost_operation, "vkWaitForFences");
    gpu::wait_idle(device);
    if (lost_without_stop(lost_operation, "vkWaitForFences")) return 1;
    fprintf(stderr, "FAIL: unknown present device-lost operation %s\n", lost_operation);
    return 1;
}

// Returns 77 when no GPU presents to the surface.
int present_frames(const char* const* extensions, VkSurfaceKHR_T* (*create_surface)(VkInstance_T*, void*), TestWindow& window, bool fixed_extent) noexcept
{
    window.surface_calls = window.extent_calls = 0;
    arm(nullptr);
    const gpu::DeviceInit init = gpu::create_device({.swapchain_format = gpu::Format::bgra8_srgb, .instance_extensions = extensions,
        .instance_extension_count = 2, .create_surface = create_surface, .drawable_extent = test_window_extent, .wsi_user = &window});
    if (init.error == gpu::Error::unsupported && !strstr(diagnostic, "create_surface")) return 77;
    check(init.device && window.surface_calls == 1, "device creation calls create_surface once with wsi_user and succeeds");
    if (!init.device) return 1;
    gpu::Device* device = init.device;
    gpu::CommandPool* pool = gpu::create_command_pool(device);
    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(device);
    check(pool && timeline, "presentation fixture is available");
    for (uint64 frame = 1; pool && timeline && frame <= 3; ++frame)
    {
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
        if (lost_operation && acquired.render_view) return lose_present_fence(device, pool, window);
    }
    const gpu::uint32x2 extent = gpu::get_drawable_extent(device);
    check(extent.x == window.extent.x && extent.y == window.extent.y, "get_drawable_extent returns the window extent");
    check(fixed_extent ? window.extent_calls == 0 : window.extent_calls != 0, "drawable_extent is asked only when the surface has no fixed extent");
    check(reports == 0, "presentation reports no diagnostic");
    gpu::wait_idle(device);
    gpu::destroy_timeline_semaphore(timeline);
    gpu::destroy_command_pool(pool);
    gpu::destroy_device(device);
    return failures ? 1 : 0;
}

int present_case() noexcept
{
    gpu::set_error_callback(receive_error);
    TestWindow window{.extent = {.x = 64, .y = 48}};
    const char* surface = "headless";
    int result = 77;
    if (instance_extension_available(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME))
        result = present_frames(headless_extensions, create_headless_surface, window, false);
#if defined(NOGRAPHICSAPI_TEST_XCB)
    if (result == 77 && instance_extension_available(VK_KHR_XCB_SURFACE_EXTENSION_NAME))
    {
        printf("Headless presentation is unavailable; trying an X11 window.\n");
        surface = "X11";
        window.connection = xcb_connect(nullptr, nullptr);
        if (!xcb_connection_has_error(window.connection))
        {
            const xcb_screen_t* screen = xcb_setup_roots_iterator(xcb_get_setup(window.connection)).data;
            window.window = xcb_generate_id(window.connection);
            xcb_create_window(window.connection, XCB_COPY_FROM_PARENT, window.window, screen->root, 0, 0, uint16(window.extent.x), uint16(window.extent.y), 0,
                XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, 0, nullptr);
            xcb_flush(window.connection);
            result = present_frames(xcb_extensions, create_xcb_surface, window, true);
            xcb_destroy_window(window.connection, window.window);
        }
        xcb_disconnect(window.connection);
    }
#endif
    gpu::set_error_callback(nullptr);
    if (!lost_operation) unchanged({});
    if (result == 77)
    {
        printf("SKIP: no GPU presents to a headless or X11 surface.\n");
        return 77;
    }
    printf("Linux presentation, %s surface: %u failures\n", surface, failures);
    return failures ? 1 : result;
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
    if (argc == 3 && !strcmp(argv[1], "--device-lost")) return device_lost_case(argv[2]);
    if (argc == 2 && !strcmp(argv[1], "--present")) return present_case();
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
