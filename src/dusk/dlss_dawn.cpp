// Dawn/NVIDIA NGX Vulkan interop for DLAA.
//
// This translation unit only exists in a `DUSK_ENABLE_DLSS` build and only
// makes sense when linked against a Dawn build that exports the NGX interop
// extensions (the patched `dawn/native/VulkanBackend.h` contract). It registers
// the extensions NGX requires *before* Dawn creates the instance/device and
// defines the `aurora_dlss_dawn_*` symbols the bridge in `dlss.cpp` probes for.
//
// When the flag is off the file compiles to nothing, so the default build has
// zero NVIDIA/Dawn-native dependency.

#ifndef DUSK_ENABLE_DLSS
#define DUSK_ENABLE_DLSS 0
#endif

#if DUSK_ENABLE_DLSS

#include "dusk/dlss.hpp"

#include <dawn/native/VulkanBackend.h>
#include <dawn/webgpu_cpp.h>

#include <aurora/gfx.hpp>

#include "dusk/logging.h"

#include <dlfcn.h>

namespace dusk::dlss {

namespace {

// Minimal probe for the NVIDIA NGX runtime. Kept local (and independent of the
// NGX SDK loader used by dlss.cpp) so this file needs no NVIDIA headers. We
// only need to know whether NGX is present: if it is not, we must not request
// its extensions, because asking Dawn to enable an extension the driver does
// not advertise would make `vkCreateDevice` fail.
bool ngx_runtime_available() {
    void* handle = dlopen("libnvidia-ngx.so.1", RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        handle = dlopen("libnvidia-ngx.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (handle == nullptr) {
        return false;
    }
    dlclose(handle);
    return true;
}

}  // namespace

void register_vulkan_extensions() {
    if (!ngx_runtime_available()) {
        // No NGX on this system (e.g. non-NVIDIA). Do nothing so the default
        // behavior of Dawn and the driver is unchanged.
        return;
    }

    const char* const instanceExtensions[] = {
        "VK_KHR_get_physical_device_properties2",
        "VK_KHR_external_memory_capabilities",
    };
    dawn::native::vulkan::RequestAdditionalInstanceExtensions(
        instanceExtensions,
        static_cast<uint32_t>(sizeof(instanceExtensions) / sizeof(instanceExtensions[0])));

    const char* const deviceExtensions[] = {
        "VK_NVX_binary_import",
        "VK_NVX_image_view_handle",
        "VK_EXT_buffer_device_address",
        "VK_KHR_push_descriptor",
    };
    dawn::native::vulkan::RequestAdditionalDeviceExtensions(
        deviceExtensions, static_cast<uint32_t>(sizeof(deviceExtensions) / sizeof(deviceExtensions[0])));

    dawn::native::vulkan::RequestAppVkQueue(true);

    DuskLog.info("DLAA: NGX present; requested additional Vulkan extensions");
    for (const char* name : instanceExtensions) {
        DuskLog.info("DLAA:   instance: {}", name);
    }
    for (const char* name : deviceExtensions) {
        DuskLog.info("DLAA:   device: {}", name);
    }
    DuskLog.info("DLAA: requested a dedicated application VkQueue");
}

// ---------------------------------------------------------------------------
// `aurora_dlss_dawn_*` interop contract.
//
// These strong definitions override the weak declarations in dlss.cpp whenever
// this TU is compiled in (i.e. a `DUSK_ENABLE_DLSS` build), telling the bridge
// that the Dawn build provides the required NGX Vulkan interop.
// ---------------------------------------------------------------------------
extern "C" {

bool aurora_dlss_dawn_supported(void) {
    // This translation unit only exists in a patched-Dawn build.
    return true;
}

void* aurora_dlss_dawn_vk_instance(void) {
    const wgpu::Device device = aurora::gfx::device();
    if (!device) {
        return nullptr;
    }
    return reinterpret_cast<void*>(dawn::native::vulkan::GetInstance(device.Get()));
}

void* aurora_dlss_dawn_vk_physical_device(void) {
    const wgpu::Device device = aurora::gfx::device();
    if (!device) {
        return nullptr;
    }
    return reinterpret_cast<void*>(dawn::native::vulkan::GetVkPhysicalDevice(device.Get()));
}

void* aurora_dlss_dawn_vk_device(void) {
    const wgpu::Device device = aurora::gfx::device();
    if (!device) {
        return nullptr;
    }
    return reinterpret_cast<void*>(dawn::native::vulkan::GetVkDevice(device.Get()));
}

void* aurora_dlss_dawn_vk_get_instance_proc_addr(void) {
    const wgpu::Device device = aurora::gfx::device();
    if (!device) {
        return nullptr;
    }
    return reinterpret_cast<void*>(
        dawn::native::vulkan::GetInstanceProcAddr(device.Get(), "vkGetInstanceProcAddr"));
}

void* aurora_dlss_dawn_vk_get_device_proc_addr(void) {
    const wgpu::Device device = aurora::gfx::device();
    if (!device) {
        return nullptr;
    }
    return reinterpret_cast<void*>(dawn::native::vulkan::GetDeviceProcAddr(device.Get(), "vkGetDeviceProcAddr"));
}

}  // extern "C"

}  // namespace dusk::dlss

#endif  // DUSK_ENABLE_DLSS
