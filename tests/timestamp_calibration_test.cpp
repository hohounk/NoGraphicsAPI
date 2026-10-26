#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <stdio.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

namespace
{
uint32 failures = 0;

void check(bool condition, const char* message) noexcept
{
    if (condition) return;
    fprintf(stderr, "FAILED: %s\n", message);
    ++failures;
}

#if !defined(__APPLE__)
// The host clock that calibrate_timestamps names on the Vulkan backend, in nanoseconds.
uint64 host_clock_ns() noexcept
{
#if defined(_WIN32)
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    const uint64 ticks = static_cast<uint64>(counter.QuadPart);
    const uint64 rate = static_cast<uint64>(frequency.QuadPart);
    return ticks / rate * 1000000000ull + ticks % rate * 1000000000ull / rate;
#else
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64>(now.tv_sec) * 1000000000ull + static_cast<uint64>(now.tv_nsec);
#endif
}
#endif
} // namespace

int main()
{
    const gpu::DeviceInit init = gpu::create_device({.timestamp_query_count = 1});
    if (init.error == gpu::Error::unsupported)
    {
        printf("SKIP: required Vulkan device features are unavailable.\n");
        return 77;
    }
    check(init.device != nullptr, "device creation succeeds");
    if (!init.device) return 1;
    const gpu::DeviceCaps& caps = gpu::get_device_caps(init.device);
    if (!caps.calibrated_timestamps || caps.timestamp_period_ns == 0.0f)
    {
        printf("SKIP: calibrated timestamps are unavailable on this device.\n");
        gpu::destroy_device(init.device);
        return 77;
    }

    const gpu::TimestampCalibration first = gpu::calibrate_timestamps(init.device);
    check(first.gpu_ticks != 0 && first.cpu_ns != 0, "a supported device returns a nonzero pair");
#if !defined(__APPLE__)
    const uint64 host_now = host_clock_ns();
    const uint64 domain_gap = host_now > first.cpu_ns ? host_now - first.cpu_ns : first.cpu_ns - host_now;
    check(domain_gap < 1000000, "cpu_ns is on the named host clock, within 1 ms of a direct read");
    printf("cpu_ns %llu, host clock %llu\n", static_cast<unsigned long long>(first.cpu_ns), static_cast<unsigned long long>(host_now));
#endif

    gpu::TimelineSemaphore* timeline = gpu::create_timeline_semaphore(init.device);
    gpu::CommandPool* pool = gpu::create_command_pool(init.device);
    gpu::CommandBuffer* commands = gpu::begin_commands(pool);
    uint64 marker = 0;
    gpu::write_timestamp(commands, &marker);
    gpu::end_commands(commands);
    gpu::CommandBuffer* const submitted[]{commands};
    gpu::submit(init.device, {.commands = submitted, .completion = {.semaphore = timeline, .value = 1}});
    gpu::wait_timeline({.semaphore = timeline, .value = 1});
    gpu::TimestampCalibration second = gpu::calibrate_timestamps(init.device);
    for (uint32 attempt = 0; attempt < 100000 && second.cpu_ns < first.cpu_ns + 20000000ull; ++attempt) second = gpu::calibrate_timestamps(init.device);
    gpu::read_timestamps(pool);

    check(second.cpu_ns > first.cpu_ns, "the host clock advances between samples");
    check(second.gpu_ticks > first.gpu_ticks, "the GPU clock advances between samples");
    check(marker != 0 && first.gpu_ticks <= marker && marker <= second.gpu_ticks, "a GPU marker written between two samples lies between their GPU ticks");
    const double gpu_delta_ns = static_cast<double>(second.gpu_ticks - first.gpu_ticks) * static_cast<double>(caps.timestamp_period_ns);
    const double cpu_delta_ns = static_cast<double>(second.cpu_ns - first.cpu_ns);
    const double period_error = gpu_delta_ns > cpu_delta_ns ? gpu_delta_ns - cpu_delta_ns : cpu_delta_ns - gpu_delta_ns;
    check(period_error <= cpu_delta_ns * 0.1, "GPU delta times the period is within 10 percent of the CPU delta");
    printf("gpu ticks %llu -> %llu (period %f ns), marker %llu, cpu ns %llu -> %llu\n",
           static_cast<unsigned long long>(first.gpu_ticks), static_cast<unsigned long long>(second.gpu_ticks),
           static_cast<double>(caps.timestamp_period_ns), static_cast<unsigned long long>(marker),
           static_cast<unsigned long long>(first.cpu_ns), static_cast<unsigned long long>(second.cpu_ns));

    gpu::wait_idle(init.device);
    gpu::destroy_command_pool(pool);
    gpu::destroy_timeline_semaphore(timeline);
    gpu::destroy_device(init.device);
    printf("Timestamp calibration: %u failures\n", failures);
    return failures ? 1 : 0;
}
