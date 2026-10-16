#include <NoGraphicsAPI/NoGraphicsAPI.hpp>

#include <stdio.h>

constexpr int skipped = 77;

int main()
{
    const gpu::DeviceInit device_init = gpu::create_device();
    if (device_init.error == gpu::Error::unsupported)
        return skipped;
    if (device_init.error != gpu::Error::none)
        return 1;

    const gpu::DeviceInfo& info = gpu::get_device_info(device_init.device);
    const gpu::DeviceMemoryInfo& memory = gpu::get_device_memory_info(device_init.device);
    const gpu::DeviceCaps& caps = gpu::get_device_caps(device_init.device);
    const bool valid_info = info.device_name && info.device_name[0] && info.driver_name && info.driver_info && info.device_name == caps.device_name &&
                            info.api_version >= ((1u << 22u) | (4u << 12u)) && info.vendor_id != 0;
    const bool valid_memory = memory.device_local_memory_size != 0 && memory.host_visible_memory_size != 0 &&
                              memory.host_visible_device_local_memory_size <= memory.device_local_memory_size &&
                              memory.host_visible_device_local_memory_size <= memory.host_visible_memory_size;
    if (!valid_info) fprintf(stderr, "FAIL: device info invariants\n");
    if (!valid_memory) fprintf(stderr, "FAIL: device memory info invariants\n");

    gpu::wait_idle(device_init.device);
    gpu::destroy_device(device_init.device);
    return valid_info && valid_memory ? 0 : 1;
}
