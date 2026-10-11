#include "dusk/dlss.hpp"

#include "dusk/logging.h"

#include <cstdint>

// Default the build flag to off so `#if DUSK_ENABLE_DLSS` is well-defined even
// when the CMake option was never enabled (and never warned about).
#ifndef DUSK_ENABLE_DLSS
#define DUSK_ENABLE_DLSS 0
#endif

// ---------------------------------------------------------------------------
// Flag ON: dynamic NGX loading + weak Dawn/Aurora Vulkan interop.
//
// The weak interop contract with the Aurora NGX bridge (or a patched Dawn):
// stock Dawn/Aurora does not export these, so they resolve to null and DLAA is
// reported unavailable. A patched Dawn (or an Aurora NGX shim) must export all
// of them for DLAA to become functional. They are deliberately declared weak so
// that the default/stock configuration links cleanly without a definition.
//
// All NVIDIA/Dawn/Vulkan headers are included only when the flag is on.
// ---------------------------------------------------------------------------
#if DUSK_ENABLE_DLSS

#if defined(__GNUC__) || defined(__clang__)
#define DUSK_DLSS_INTEROP_WEAK 1
extern "C" {
__attribute__((weak)) bool aurora_dlss_dawn_supported(void);
__attribute__((weak)) void* aurora_dlss_dawn_vk_instance(void);         // VkInstance
__attribute__((weak)) void* aurora_dlss_dawn_vk_physical_device(void);  // VkPhysicalDevice
__attribute__((weak)) void* aurora_dlss_dawn_vk_device(void);           // VkDevice
__attribute__((weak)) void* aurora_dlss_dawn_vk_get_instance_proc_addr(void);  // PFN_vkGetInstanceProcAddr
__attribute__((weak)) void* aurora_dlss_dawn_vk_get_device_proc_addr(void);    // PFN_vkGetDeviceProcAddr
}
#else
// Toolchains without ELF/Mach-O weak symbols: use local null stubs so the probe
// fails cleanly instead of failing to link.
#define DUSK_DLSS_INTEROP_WEAK 0
static bool aurora_dlss_dawn_supported(void) {
    return false;
}
static void* aurora_dlss_dawn_vk_instance(void) {
    return nullptr;
}
static void* aurora_dlss_dawn_vk_physical_device(void) {
    return nullptr;
}
static void* aurora_dlss_dawn_vk_device(void) {
    return nullptr;
}
static void* aurora_dlss_dawn_vk_get_instance_proc_addr(void) {
    return nullptr;
}
static void* aurora_dlss_dawn_vk_get_device_proc_addr(void) {
    return nullptr;
}
#endif

#include <vulkan/vulkan.h>

// All NGX entry points are provided by the SDK's forwarding loader stub
// (`libnvsdk_ngx.a`, linked for DUSK_ENABLE_DLSS builds), which internally
// dlopens the NGX runtime and forwards every call to it. The runtime driver
// library itself does NOT export `NVSDK_NGX_VULKAN_Init_with_ProjectID` (only
// `NVSDK_NGX_VULKAN_Init_ProjectID`), so the stub is mandatory for init; binding
// the rest through the stub as well keeps a single consistent call path (and
// avoids crashing on the runtime's raw `NVSDK_NGX_VULKAN_Shutdown1`).
//
// `NVSDK_NGX_HEADER_ONLY` keeps the SDK's small dlopen/dlsym helpers
// (NGXLoadCoreLibrary / NGXGetLibrarySymbol) available for the runtime-presence
// probe used by `is_supported()`. The archive's own loader uses mangled C++
// names, so the C-symbol helpers defined by the header do not clash with it (no
// ODR/duplicate-symbol issue).
#define NVSDK_NGX_HEADER_ONLY
#define NVSDK_NGX_NO_CUDA
#include <nvsdk_ngx_loader.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>

// Patched Dawn Vulkan backend getters (GetVkDevice / GetAppVkQueue /
// GetQueueFamilyIndex / GetDeviceProcAddr). The default Dawn package does not
// declare these, so this TU and `dlss_dawn.cpp` must only be built against the
// patched Dawn package (see CONTEXT.md).
#include <dawn/native/VulkanBackend.h>

#include <aurora/gfx.hpp>

#include "d/d_com_inf_game.h"
#include "dusk/data.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

#endif  // DUSK_ENABLE_DLSS

namespace dusk::dlss {

namespace {

// Halton(2,3) jitter phase count. DLSS consumes the matching offset passed to
// NVSDK_NGX_VK_DLSS_Eval_Params::InJitterOffsetX/Y, so keeping this fixed means
// the projection jitter and the evaluator stay in lockstep.
constexpr uint32_t kJitterPhaseCount = 8;

// Van der Corput / Halton radical inverse. Pure math, no allocation.
constexpr float halton(uint32_t index, uint32_t base) {
    float result = 0.0f;
    float f = 1.0f;
    uint32_t i = index;
    while (i > 0) {
        f /= static_cast<float>(base);
        result += f * static_cast<float>(i % base);
        i /= base;
    }
    return result;
}

}  // namespace

void get_jitter(uint64_t frameIndex, float* outX, float* outY) {
    if (outX == nullptr || outY == nullptr) {
        return;
    }
    const uint32_t index = static_cast<uint32_t>(frameIndex % kJitterPhaseCount);
    *outX = halton(index, 2) - 0.5f;
    *outY = halton(index, 3) - 0.5f;
}

// ===========================================================================
// Flag OFF: inert stubs. No NVIDIA headers are included in this translation
// unit when the flag is off.
// ===========================================================================
#if !DUSK_ENABLE_DLSS

void register_vulkan_extensions() {}

bool is_supported() {
    return false;
}

const char* unavailable_reason() {
    return "DLSS support was not compiled in.";
}

bool initialize() {
    return false;
}

void shutdown() {}

bool create(uint32_t /*renderWidth*/, uint32_t /*renderHeight*/, Quality /*quality*/) {
    return false;
}

void release() {}

void set_reset(bool /*reset*/) {}

void advance_jitter() {}

#else  // DUSK_ENABLE_DLSS

namespace {

constexpr uint32_t kNvidiaVendorId = 0x10DE;

// Placeholder identifiers. NGX wants a GUID-like project ID for
// NVSDK_NGX_ENGINE_TYPE_CUSTOM; these are not registered with NVIDIA and exist
// only to satisfy the API. Replace with a real application ID if/when Dusklight
// ships DLSS publicly.
constexpr char kProjectId[] = "b2f7c1e0-4a3d-4e8b-9c6f-1d2e3a4b5c6d";
constexpr char kEngineVersion[] = "1.0.0";

// NGX entry points, all bound to the SDK's loader stub (`libnvsdk_ngx.a`, linked
// for DUSK_ENABLE_DLSS builds). The stub dlopens the NGX runtime on demand and
// forwards every call to it. Two reasons to go through the stub instead of
// dlsym-ing the runtime directly:
//
//   * The runtime driver library does not export
//     `NVSDK_NGX_VULKAN_Init_with_ProjectID` at all (only `..._Init_ProjectID`),
//     so that symbol is only available from the stub.
//   * Calling the runtime's raw `NVSDK_NGX_VULKAN_Shutdown1` directly crashes
//     inside libnvidia-ngx.so on current drivers; the stub's wrapper shuts down
//     cleanly.
//
// `handle` is only used to probe that the NGX runtime exists (so
// `is_supported()` can report a clear reason before the device exists).
struct NgxApi {
    void* handle = nullptr;
    decltype(&NVSDK_NGX_VULKAN_Init_with_ProjectID) init = &NVSDK_NGX_VULKAN_Init_with_ProjectID;
    decltype(&NVSDK_NGX_VULKAN_GetCapabilityParameters) getCapabilityParameters =
        &NVSDK_NGX_VULKAN_GetCapabilityParameters;
    decltype(&NVSDK_NGX_VULKAN_DestroyParameters) destroyParameters = &NVSDK_NGX_VULKAN_DestroyParameters;
    decltype(&NVSDK_NGX_VULKAN_CreateFeature1) createFeature = &NVSDK_NGX_VULKAN_CreateFeature1;
    decltype(&NVSDK_NGX_VULKAN_EvaluateFeature) evaluateFeature = &NVSDK_NGX_VULKAN_EvaluateFeature;
    decltype(&NVSDK_NGX_VULKAN_ReleaseFeature) releaseFeature = &NVSDK_NGX_VULKAN_ReleaseFeature;
    decltype(&NVSDK_NGX_VULKAN_Shutdown1) shutdown = &NVSDK_NGX_VULKAN_Shutdown1;
};

NgxApi g_ngx;
std::once_flag g_ngxOnce;

void load_ngx_once() {
    // Probe for the runtime so `is_supported()` can report a clear reason when
    // NGX is absent (e.g. non-NVIDIA systems). The actual entry points are
    // linked in via the SDK loader stub above.
    g_ngx.handle = NGXLoadCoreLibrary(NVSDK_NGX_LUID{0, 0});
    if (g_ngx.handle == nullptr) {
        // Fall back to the unversioned soname if the versioned one is absent.
        g_ngx.handle = NGXLoadLibrary("libnvidia-ngx.so");
    }
}

// First-present interop function pointer, or null when absent.
template <typename Fn>
void* call_interop(Fn fn) {
    return fn ? fn() : nullptr;
}

std::mutex g_probeMutex;
bool g_probeDone = false;
bool g_supported = false;
const char* g_reason = "DLSS support was not compiled in.";

bool evaluate_support() {
    std::lock_guard<std::mutex> lock(g_probeMutex);
    if (g_probeDone) {
        return g_supported;
    }

    std::call_once(g_ngxOnce, load_ngx_once);
    if (g_ngx.handle == nullptr) {
        g_reason = "NVIDIA NGX runtime not found.";
        g_probeDone = true;
        return false;
    }

    if (!DUSK_DLSS_INTEROP_WEAK || aurora_dlss_dawn_supported == nullptr || !aurora_dlss_dawn_supported()) {
        g_reason = "A Dawn build with NGX Vulkan interop is required.";
        g_probeDone = true;
        return false;
    }

    const wgpu::Device device = aurora::gfx::device();
    if (!device) {
        // Not a definitive answer: the render device does not exist yet. Do not
        // cache, so a later call (after device creation) can succeed.
        g_reason = "Graphics device is not initialized.";
        return false;
    }

    const wgpu::Adapter adapter = device.GetAdapter();
    if (!adapter) {
        g_reason = "No graphics adapter is available.";
        g_probeDone = true;
        return false;
    }

    wgpu::AdapterInfo info{};
    adapter.GetInfo(&info);
    if (info.backendType != wgpu::BackendType::Vulkan) {
        g_reason = "DLAA requires the Vulkan backend.";
        g_probeDone = true;
        return false;
    }
    if (info.vendorID != kNvidiaVendorId) {
        g_reason = "No NVIDIA GPU detected.";
        g_probeDone = true;
        return false;
    }

    g_supported = true;
    g_reason = "";
    g_probeDone = true;
    return true;
}

bool g_initialized = false;
void* g_vkDevice = nullptr;
NVSDK_NGX_Parameter* g_params = nullptr;
NVSDK_NGX_Handle* g_feature = nullptr;
[[maybe_unused]] bool g_reset = true;

// ---------------------------------------------------------------------------
// Minimal Vulkan command-buffer infrastructure for NGX.
//
// NGX's `NVSDK_NGX_VULKAN_CreateFeature1` requires an *open and recording*
// command buffer; passing `VK_NULL_HANDLE` (as the previous prototype did)
// makes NGX dereference a null command buffer inside `libnvidia-glcore` and
// SIGSEGV. We therefore own a tiny, self-contained command pool / command
// buffer on Dawn's reserved application queue (requested via
// `RequestAppVkQueue(true)`; queue index 1 of Dawn's main queue family) and
// pass a recording command buffer to NGX.
//
// This is deliberately simple and synchronous (one command buffer, submit +
// `vkQueueWaitIdle`). It is a prototype; an evaluate path would want proper
// semaphores/fences and to interleave with Dawn's own submissions. The
// structure (`begin()` / `end_and_submit()`) is enough for `create()` today and
// can be reused by `evaluate()` later.
//
// Everything degrades to a no-op/false (never crashes) when a handle or a
// device-level proc is unavailable.
// ---------------------------------------------------------------------------
struct VkCommandInfra {
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
    bool recording = false;

    PFN_vkCreateCommandPool createCommandPool = nullptr;
    PFN_vkDestroyCommandPool destroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers allocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer beginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer endCommandBuffer = nullptr;
    PFN_vkQueueSubmit queueSubmit = nullptr;
    PFN_vkQueueWaitIdle queueWaitIdle = nullptr;

    // Extra device-level procs used by the evaluate path (image views + clears)
    // and for whole-device synchronization with Dawn's own queue submissions.
    PFN_vkDeviceWaitIdle deviceWaitIdle = nullptr;
    PFN_vkCreateImageView createImageView = nullptr;
    PFN_vkDestroyImageView destroyImageView = nullptr;
    PFN_vkCmdClearColorImage cmdClearColorImage = nullptr;

    // Lazily resolves the device handles/procs and creates the pool + one
    // primary command buffer. Returns false (safe no-op) on any failure.
    bool ensure_ready() {
        if (cmdBuffer != VK_NULL_HANDLE) {
            return true;
        }

        const wgpu::Device wgpuDevice = aurora::gfx::device();
        if (!wgpuDevice) {
            return false;
        }
        const WGPUDevice nativeDevice = wgpuDevice.Get();

        if (device == VK_NULL_HANDLE) {
            device = dawn::native::vulkan::GetVkDevice(nativeDevice);
            queue = dawn::native::vulkan::GetAppVkQueue(nativeDevice);
            queueFamily = dawn::native::vulkan::GetQueueFamilyIndex(nativeDevice);

            const auto resolve = [nativeDevice](const char* name) -> PFN_vkVoidFunction {
                return dawn::native::vulkan::GetDeviceProcAddr(nativeDevice, name);
            };
            createCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(resolve("vkCreateCommandPool"));
            destroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(resolve("vkDestroyCommandPool"));
            allocateCommandBuffers =
                reinterpret_cast<PFN_vkAllocateCommandBuffers>(resolve("vkAllocateCommandBuffers"));
            beginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(resolve("vkBeginCommandBuffer"));
            endCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(resolve("vkEndCommandBuffer"));
            queueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(resolve("vkQueueSubmit"));
            queueWaitIdle = reinterpret_cast<PFN_vkQueueWaitIdle>(resolve("vkQueueWaitIdle"));
            deviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(resolve("vkDeviceWaitIdle"));
            createImageView = reinterpret_cast<PFN_vkCreateImageView>(resolve("vkCreateImageView"));
            destroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(resolve("vkDestroyImageView"));
            cmdClearColorImage = reinterpret_cast<PFN_vkCmdClearColorImage>(resolve("vkCmdClearColorImage"));
        }

        if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE || createCommandPool == nullptr ||
            destroyCommandPool == nullptr || allocateCommandBuffers == nullptr || beginCommandBuffer == nullptr ||
            endCommandBuffer == nullptr || queueSubmit == nullptr || queueWaitIdle == nullptr) {
            DuskLog.warn("dlss: Vulkan command infrastructure unavailable (missing handle/proc)");
            return false;
        }

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily;
        if (createCommandPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS || pool == VK_NULL_HANDLE) {
            DuskLog.warn("dlss: vkCreateCommandPool failed");
            return false;
        }

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        if (allocateCommandBuffers(device, &allocInfo, &cmdBuffer) != VK_SUCCESS || cmdBuffer == VK_NULL_HANDLE) {
            DuskLog.warn("dlss: vkAllocateCommandBuffers failed");
            return false;
        }

        DuskLog.info("dlss: created Vulkan command pool/buffer (queue family {})", queueFamily);
        return true;
    }

    // Begins recording `cmdBuffer` for a one-time submit. The pool was created
    // with RESET_COMMAND_BUFFER_BIT, so re-beginning implicitly resets a
    // previously recorded buffer (safe to call repeatedly, e.g. for evaluate).
    bool begin() {
        if (!ensure_ready()) {
            return false;
        }
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (beginCommandBuffer(cmdBuffer, &beginInfo) != VK_SUCCESS) {
            DuskLog.warn("dlss: vkBeginCommandBuffer failed");
            return false;
        }
        recording = true;
        return true;
    }

    // Ends recording, submits to the app queue and waits. Simple synchronous
    // prototype sync (blocks the caller until the GPU is idle on that queue).
    bool end_and_submit() {
        if (!recording) {
            return false;
        }
        recording = false;
        if (endCommandBuffer(cmdBuffer) != VK_SUCCESS) {
            DuskLog.warn("dlss: vkEndCommandBuffer failed");
            return false;
        }
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmdBuffer;
        if (queueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS) {
            DuskLog.warn("dlss: vkQueueSubmit failed");
            return false;
        }
        if (queueWaitIdle(queue) != VK_SUCCESS) {
            DuskLog.warn("dlss: vkQueueWaitIdle failed");
            return false;
        }
        return true;
    }

    void shutdown() {
        if (device != VK_NULL_HANDLE && pool != VK_NULL_HANDLE && destroyCommandPool != nullptr) {
            destroyCommandPool(device, pool, nullptr);
        }
        pool = VK_NULL_HANDLE;
        cmdBuffer = VK_NULL_HANDLE;
        recording = false;
    }
};

VkCommandInfra g_cmd;

// ---------------------------------------------------------------------------
// DLAA evaluate resources.
//
// The scene color/depth are borrowed from Aurora (Dawn-owned); NGX only reads
// them, so their Dawn images/views are reused directly. The output and the MVP
// (zeroed) motion-vector textures are owned here: NGX writes the output and
// samples the MV buffer, and Aurora's present path samples the output. They are
// recreated when the render size or scene color format changes.
// ---------------------------------------------------------------------------
VkFormat to_vk_format(wgpu::TextureFormat format) {
    switch (format) {
    case wgpu::TextureFormat::R8Unorm:
        return VK_FORMAT_R8_UNORM;
    case wgpu::TextureFormat::R8Snorm:
        return VK_FORMAT_R8_SNORM;
    case wgpu::TextureFormat::R8Uint:
        return VK_FORMAT_R8_UINT;
    case wgpu::TextureFormat::R8Sint:
        return VK_FORMAT_R8_SINT;
    case wgpu::TextureFormat::R16Uint:
        return VK_FORMAT_R16_UINT;
    case wgpu::TextureFormat::R16Sint:
        return VK_FORMAT_R16_SINT;
    case wgpu::TextureFormat::R16Float:
        return VK_FORMAT_R16_SFLOAT;
    case wgpu::TextureFormat::RG8Unorm:
        return VK_FORMAT_R8G8_UNORM;
    case wgpu::TextureFormat::RG8Snorm:
        return VK_FORMAT_R8G8_SNORM;
    case wgpu::TextureFormat::RG8Uint:
        return VK_FORMAT_R8G8_UINT;
    case wgpu::TextureFormat::RG8Sint:
        return VK_FORMAT_R8G8_SINT;
    case wgpu::TextureFormat::R32Uint:
        return VK_FORMAT_R32_UINT;
    case wgpu::TextureFormat::R32Sint:
        return VK_FORMAT_R32_SINT;
    case wgpu::TextureFormat::R32Float:
        return VK_FORMAT_R32_SFLOAT;
    case wgpu::TextureFormat::RG16Uint:
        return VK_FORMAT_R16G16_UINT;
    case wgpu::TextureFormat::RG16Sint:
        return VK_FORMAT_R16G16_SINT;
    case wgpu::TextureFormat::RG16Float:
        return VK_FORMAT_R16G16_SFLOAT;
    case wgpu::TextureFormat::RGBA8Unorm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case wgpu::TextureFormat::RGBA8UnormSrgb:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case wgpu::TextureFormat::RGBA8Snorm:
        return VK_FORMAT_R8G8B8A8_SNORM;
    case wgpu::TextureFormat::RGBA8Uint:
        return VK_FORMAT_R8G8B8A8_UINT;
    case wgpu::TextureFormat::RGBA8Sint:
        return VK_FORMAT_R8G8B8A8_SINT;
    case wgpu::TextureFormat::BGRA8Unorm:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case wgpu::TextureFormat::BGRA8UnormSrgb:
        return VK_FORMAT_B8G8R8A8_SRGB;
    case wgpu::TextureFormat::RGB10A2Unorm:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case wgpu::TextureFormat::RG11B10Ufloat:
        return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case wgpu::TextureFormat::RGB9E5Ufloat:
        return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case wgpu::TextureFormat::RG32Uint:
        return VK_FORMAT_R32G32_UINT;
    case wgpu::TextureFormat::RG32Sint:
        return VK_FORMAT_R32G32_SINT;
    case wgpu::TextureFormat::RG32Float:
        return VK_FORMAT_R32G32_SFLOAT;
    case wgpu::TextureFormat::RGBA16Uint:
        return VK_FORMAT_R16G16B16A16_UINT;
    case wgpu::TextureFormat::RGBA16Sint:
        return VK_FORMAT_R16G16B16A16_SINT;
    case wgpu::TextureFormat::RGBA16Float:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case wgpu::TextureFormat::RGBA32Uint:
        return VK_FORMAT_R32G32B32A32_UINT;
    case wgpu::TextureFormat::RGBA32Sint:
        return VK_FORMAT_R32G32B32A32_SINT;
    case wgpu::TextureFormat::RGBA32Float:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case wgpu::TextureFormat::Depth16Unorm:
        return VK_FORMAT_D16_UNORM;
    case wgpu::TextureFormat::Depth24Plus:
        return VK_FORMAT_X8_D24_UNORM_PACK32;
    case wgpu::TextureFormat::Depth24PlusStencil8:
        return VK_FORMAT_D24_UNORM_S8_UINT;
    case wgpu::TextureFormat::Depth32Float:
        return VK_FORMAT_D32_SFLOAT;
    case wgpu::TextureFormat::Depth32FloatStencil8:
        return VK_FORMAT_D32_SFLOAT_S8_UINT;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

struct DlaaResources {
    uint32_t width = 0;
    uint32_t height = 0;
    wgpu::TextureFormat colorFormat = wgpu::TextureFormat::Undefined;

    wgpu::Texture outputTexture;
    wgpu::TextureView outputView;
    VkImage outputImage = VK_NULL_HANDLE;
    VkImageView outputImageView = VK_NULL_HANDLE;
    VkFormat outputVkFormat = VK_FORMAT_UNDEFINED;

    wgpu::Texture mvTexture;
    wgpu::TextureView mvView;
    VkImage mvImage = VK_NULL_HANDLE;
    VkImageView mvImageView = VK_NULL_HANDLE;
    VkFormat mvVkFormat = VK_FORMAT_UNDEFINED;

    bool matches(uint32_t w, uint32_t h, wgpu::TextureFormat format) const {
        return outputImageView != VK_NULL_HANDLE && mvImageView != VK_NULL_HANDLE && width == w && height == h &&
               colorFormat == format;
    }
};

DlaaResources g_resources;
// Guards the feature handle + resources against the game thread (create/release
// in syncAntiAliasing) racing the render worker (the evaluate hook).
std::mutex g_dlaaMutex;

// Per-frame sub-pixel jitter (render-pixel units), shared between Aurora's
// projection jitter and the NGX evaluate call. Written by the game thread,
// read by the render worker.
std::atomic<float> g_jitterX{0.f};
std::atomic<float> g_jitterY{0.f};
uint64_t g_jitterFrame = 0;
bool g_evalLogged = false;
uint32_t g_evalFailCount = 0;

// ===========================================================================
// Camera-reprojection motion vectors (the §7.1 MVP).
//
// DLSS/DLAA motion vectors describe where each pixel moved from the previous
// frame to the current one, in render-resolution pixels, with the DLSS
// convention `motionVector = previousPixel - currentPixel` (so
// `currentPixel + motionVector` is the location the surface occupied in the
// previous frame; this is what the DLSS Programming Guide specifies). +y points
// down, matching the render target / UV orientation.
//
// Camera-only reprojection: for a static surface the world position is the
// same in both frames, so the previous clip position is
//     clipPrev = M_eff_prev * inverse(M_eff_curr) * clipCur
// where M_eff is the effective (reversed-Z) world->clip matrix. That constant
// `C = M_eff_prev * inverse(M_eff_curr)` is computed here on the CPU and fed to
// a full-screen pass that reconstructs clipCur from the depth buffer.
//
// This ignores object motion (characters/particles will ghost); DLSS mitigates
// that with its depth-based clamping. Full per-object MVs are a follow-up.
// ===========================================================================

// 4x4 row-major matrix in the game/Aurora convention (`clip = M * world`,
// vectors as columns), matching `Mtx44`.
using Mat4 = float[4][4];

void mat4_identity(Mat4 out) {
    std::memset(out, 0, sizeof(Mat4));
    for (int i = 0; i < 4; ++i) {
        out[i][i] = 1.0f;
    }
}

// `projView = proj * view`. Aurora's GX shader adjusts the projection for the
// depth range right before uploading it: reversed-Z negates the projection's
// row 2, forward-Z adds row 3 to row 2 (`extern/aurora/lib/gx/shader_info.cpp`,
// the single projection upload choke point). Because projView = proj * view,
// that is equivalent to applying the same row operation to `projView`'s row 2,
// which is what this reproduces so the matrix matches the stored depth.
void mat4_effective_projview(const Mtx44 projView, Mat4 out) {
    std::memcpy(out, projView, sizeof(Mat4));
    if (aurora::gfx::uses_reversed_z()) {
        for (int c = 0; c < 4; ++c) {
            out[2][c] = -out[2][c];
        }
    } else {
        for (int c = 0; c < 4; ++c) {
            out[2][c] = out[2][c] + out[3][c];
        }
    }
}

void mat4_mul(const Mat4 a, const Mat4 b, Mat4 out) {
    Mat4 tmp;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            tmp[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c] + a[r][3] * b[3][c];
        }
    }
    std::memcpy(out, tmp, sizeof(Mat4));
}

// Gauss-Jordan inverse with partial pivoting. Returns false when singular.
bool mat4_inverse(const Mat4 m, Mat4 out) {
    float a[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r][c] = m[r][c];
            a[r][c + 4] = (r == c) ? 1.0f : 0.0f;
        }
    }
    for (int col = 0; col < 4; ++col) {
        int pivot = col;
        float best = std::fabs(a[col][col]);
        for (int r = col + 1; r < 4; ++r) {
            const float v = std::fabs(a[r][col]);
            if (v > best) {
                best = v;
                pivot = r;
            }
        }
        if (best < 1e-20f) {
            return false;
        }
        if (pivot != col) {
            for (int c = 0; c < 8; ++c) {
                std::swap(a[pivot][c], a[col][c]);
            }
        }
        const float invPivot = 1.0f / a[col][col];
        for (int c = 0; c < 8; ++c) {
            a[col][c] *= invPivot;
        }
        for (int r = 0; r < 4; ++r) {
            if (r == col) {
                continue;
            }
            const float f = a[r][col];
            if (f == 0.0f) {
                continue;
            }
            for (int c = 0; c < 8; ++c) {
                a[r][c] -= f * a[col][c];
            }
        }
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r][c] = a[r][c + 4];
        }
    }
    return true;
}

// Largest absolute element distance of `m` from the identity. Used to detect
// camera cuts: a normal per-frame reprojection stays near the identity, while a
// cut/teleport produces a wildly different matrix. Returns a huge value when
// non-finite so a degenerate matrix is treated as a cut.
float mat4_identity_delta(const Mat4 m) {
    float maxDelta = 0.0f;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const float target = (r == c) ? 1.0f : 0.0f;
            const float d = std::fabs(m[r][c] - target);
            if (!std::isfinite(d)) {
                return 1e30f;
            }
            maxDelta = std::max(maxDelta, d);
        }
    }
    return maxDelta;
}

// A reprojection whose constant is further from identity than this is treated
// as a camera cut (observed callbacks for real per-frame motion stay < ~1).
constexpr float kMvCutThreshold = 3.0f;

// Uniform for the motion-vector pass: the four rows of C plus the render size.
struct MvUniforms {
    float c[4][4];
    float resolution[2];
    float pad[2];
};
static_assert(sizeof(MvUniforms) == 80);

// Full-screen pass. `depth_tex` is the scene depth (`rg16float` is not a WebGPU
// storage format, so this is a fragment pass into the RG16F MV target rather
// than a compute/storage pass). Motion vectors are emitted in render pixels,
// previous - current, +y down.
constexpr std::string_view kMvShader = R"(
struct MvUniforms {
    c0: vec4f,
    c1: vec4f,
    c2: vec4f,
    c3: vec4f,
    resolution: vec2f,
    pad: vec2f,
};

@group(0) @binding(0) var depth_tex: texture_depth_2d;
@group(0) @binding(1) var<uniform> u: MvUniforms;

@vertex
fn vs_main(@builtin(vertex_index) vi: u32) -> @builtin(position) vec4f {
    var p = array<vec2f, 3>(vec2f(-1.0, 1.0), vec2f(-1.0, -3.0), vec2f(3.0, 1.0));
    return vec4f(p[vi], 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) frag: vec4f) -> @location(0) vec2f {
    let pixel = vec2i(frag.xy);
    let depth = textureLoad(depth_tex, pixel, 0);
    let uc = frag.xy / u.resolution;
    // NDC y points up while the framebuffer/depth texture y points down.
    let ndc_cur = vec2f(uc.x * 2.0 - 1.0, 1.0 - uc.y * 2.0);
    let clip_cur = vec4f(ndc_cur.x, ndc_cur.y, depth, 1.0);
    let clip_prev = vec4f(dot(u.c0, clip_cur), dot(u.c1, clip_cur),
                          dot(u.c2, clip_cur), dot(u.c3, clip_cur));
    let ndc_prev = clip_prev.xy / clip_prev.w;
    // DLSS convention: previous - current, in render pixels, +y down.
    let mv = vec2f(ndc_prev.x - ndc_cur.x, ndc_cur.y - ndc_prev.y) * u.resolution * 0.5;
    return mv;
}
)";

struct MvPassState {
    wgpu::BindGroupLayout layout;
    wgpu::RenderPipeline pipeline;
    wgpu::Buffer uniform;
    float prevEff[4][4]{};
    bool hasPrev = false;
};

MvPassState g_mv;

// Lazily creates the (device-lifetime) motion-vector pipeline/uniform. The
// render target/view are per-size and passed in per frame.
bool ensure_mv_pipeline() {
    if (g_mv.pipeline) {
        return true;
    }
    const wgpu::Device device = aurora::gfx::device();
    if (!device) {
        return false;
    }

    const std::array layoutEntries{
        wgpu::BindGroupLayoutEntry{
            .binding = 0,
            .visibility = wgpu::ShaderStage::Fragment,
            .texture =
                wgpu::TextureBindingLayout{
                    .sampleType = wgpu::TextureSampleType::Depth,
                    .viewDimension = wgpu::TextureViewDimension::e2D,
                },
        },
        wgpu::BindGroupLayoutEntry{
            .binding = 1,
            .visibility = wgpu::ShaderStage::Fragment,
            .buffer =
                wgpu::BufferBindingLayout{
                    .type = wgpu::BufferBindingType::Uniform,
                },
        },
    };
    const wgpu::BindGroupLayoutDescriptor layoutDesc{
        .label = "DLAA motion-vector bind group layout",
        .entryCount = layoutEntries.size(),
        .entries = layoutEntries.data(),
    };
    g_mv.layout = device.CreateBindGroupLayout(&layoutDesc);

    const std::string source{kMvShader};
    const wgpu::ShaderSourceWGSL wgslSource{wgpu::ShaderSourceWGSL::Init{.code = source.c_str()}};
    const wgpu::ShaderModuleDescriptor moduleDesc{
        .nextInChain = &wgslSource,
        .label = "DLAA motion-vector shader",
    };
    const auto module = device.CreateShaderModule(&moduleDesc);

    const std::array colorTargets{wgpu::ColorTargetState{
        .format = wgpu::TextureFormat::RG16Float,
    }};
    const wgpu::FragmentState fragmentState{
        .module = module,
        .entryPoint = "fs_main",
        .targetCount = colorTargets.size(),
        .targets = colorTargets.data(),
    };
    const wgpu::PipelineLayoutDescriptor pipelineLayoutDesc{
        .bindGroupLayoutCount = 1,
        .bindGroupLayouts = &g_mv.layout,
    };
    const auto pipelineLayout = device.CreatePipelineLayout(&pipelineLayoutDesc);
    const wgpu::RenderPipelineDescriptor pipelineDesc{
        .label = "DLAA motion-vector pipeline",
        .layout = pipelineLayout,
        .vertex =
            wgpu::VertexState{
                .module = module,
                .entryPoint = "vs_main",
            },
        .primitive =
            wgpu::PrimitiveState{
                .topology = wgpu::PrimitiveTopology::TriangleList,
            },
        .fragment = &fragmentState,
    };
    g_mv.pipeline = device.CreateRenderPipeline(&pipelineDesc);

    const wgpu::BufferDescriptor uniformDesc{
        .label = "DLAA motion-vector uniforms",
        .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
        .size = sizeof(MvUniforms),
    };
    g_mv.uniform = device.CreateBuffer(&uniformDesc);
    return g_mv.pipeline && g_mv.uniform;
}

// Records and submits the reprojection pass. `C` is `M_eff_prev * inverse(M_eff_curr)`
// (identity on a reset frame, which yields zero MVs). Returns true when the pass
// was submitted.
bool run_mv_pass(const wgpu::TextureView& depthView, const Mat4 C, uint32_t width, uint32_t height) {
    if (!ensure_mv_pipeline()) {
        return false;
    }
    const wgpu::Device device = aurora::gfx::device();
    const wgpu::Queue queue = aurora::gfx::queue();
    if (!device || !queue) {
        return false;
    }

    MvUniforms uniforms{};
    std::memcpy(uniforms.c, C, sizeof(uniforms.c));
    uniforms.resolution[0] = static_cast<float>(width);
    uniforms.resolution[1] = static_cast<float>(height);
    queue.WriteBuffer(g_mv.uniform, 0, &uniforms, sizeof(uniforms));

    const std::array bindEntries{
        wgpu::BindGroupEntry{
            .binding = 0,
            .textureView = depthView,
        },
        wgpu::BindGroupEntry{
            .binding = 1,
            .buffer = g_mv.uniform,
            .size = sizeof(MvUniforms),
        },
    };
    const wgpu::BindGroupDescriptor bindGroupDesc{
        .label = "DLAA motion-vector bind group",
        .layout = g_mv.layout,
        .entryCount = bindEntries.size(),
        .entries = bindEntries.data(),
    };
    const auto bindGroup = device.CreateBindGroup(&bindGroupDesc);

    const std::array colorAttachments{
        wgpu::RenderPassColorAttachment{
            .view = g_resources.mvView,
            .loadOp = wgpu::LoadOp::Clear,
            .storeOp = wgpu::StoreOp::Store,
            .clearValue = {0.f, 0.f, 0.f, 0.f},
        },
    };
    const wgpu::RenderPassDescriptor renderPassDesc{
        .label = "DLAA motion-vector pass",
        .colorAttachmentCount = colorAttachments.size(),
        .colorAttachments = colorAttachments.data(),
    };
    const auto encoder = device.CreateCommandEncoder();
    const auto pass = encoder.BeginRenderPass(&renderPassDesc);
    pass.SetPipeline(g_mv.pipeline);
    pass.SetBindGroup(0, bindGroup);
    pass.Draw(3);
    pass.End();

    const auto cmd = encoder.Finish();
    queue.Submit(1, &cmd);
    return true;
}

VkImageView create_color_image_view(VkImage image, VkFormat format) {
    if (image == VK_NULL_HANDLE || format == VK_FORMAT_UNDEFINED || g_cmd.createImageView == nullptr) {
        return VK_NULL_HANDLE;
    }
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.baseMipLevel = 0;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.baseArrayLayer = 0;
    info.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    if (g_cmd.createImageView(g_cmd.device, &info, nullptr, &view) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return view;
}

void release_resources() {
    if (g_cmd.destroyImageView != nullptr && g_cmd.device != VK_NULL_HANDLE) {
        if (g_resources.outputImageView != VK_NULL_HANDLE) {
            g_cmd.destroyImageView(g_cmd.device, g_resources.outputImageView, nullptr);
        }
        if (g_resources.mvImageView != VK_NULL_HANDLE) {
            g_cmd.destroyImageView(g_cmd.device, g_resources.mvImageView, nullptr);
        }
    }
    g_resources = {};
    // A new MV target invalidates the reprojection history (no previous frame).
    g_mv.hasPrev = false;
    std::memset(g_mv.prevEff, 0, sizeof(g_mv.prevEff));
}

bool ensure_resources(uint32_t width, uint32_t height, wgpu::TextureFormat colorFormat) {
    if (g_resources.matches(width, height, colorFormat)) {
        return true;
    }
    release_resources();

    const VkFormat colorVk = to_vk_format(colorFormat);
    if (colorVk == VK_FORMAT_UNDEFINED || g_cmd.createImageView == nullptr) {
        DuskLog.warn("dlss: unsupported scene color format for DLAA ({})", static_cast<int>(colorFormat));
        return false;
    }

    const wgpu::Device device = aurora::gfx::device();
    if (!device) {
        return false;
    }
    const auto make_texture = [&device](const char* label, wgpu::TextureFormat format, wgpu::TextureUsage usage,
                                        uint32_t w, uint32_t h) {
        const wgpu::TextureDescriptor desc{
            .label = label,
            .usage = usage,
            .dimension = wgpu::TextureDimension::e2D,
            .size = {w, h, 1},
            .format = format,
            .mipLevelCount = 1,
            .sampleCount = 1,
        };
        return device.CreateTexture(&desc);
    };

    // Output: same format/size as the scene color so Aurora's present blit is a
    // straight copy. NGX writes it as a storage image.
    g_resources.outputTexture =
        make_texture("DLAA output", colorFormat,
                     wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::StorageBinding |
                         wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst,
                     width, height);
    // Motion vectors: RG16Float. NGX reads them; the camera-reprojection pass
    // renders into them (RenderAttachment). RG16Float is not a WebGPU storage
    // format, so the pass is a fragment pass rather than compute/storage.
    g_resources.mvTexture =
        make_texture("DLAA motion vectors", wgpu::TextureFormat::RG16Float,
                     wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                         wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst,
                     width, height);

    if (!g_resources.outputTexture || !g_resources.mvTexture) {
        DuskLog.warn("dlss: failed to create DLAA output/motion-vector textures");
        release_resources();
        return false;
    }

    g_resources.outputView = g_resources.outputTexture.CreateView();
    g_resources.mvView = g_resources.mvTexture.CreateView();
    g_resources.outputImage = dawn::native::vulkan::GetVkImage(g_resources.outputTexture.Get());
    g_resources.mvImage = dawn::native::vulkan::GetVkImage(g_resources.mvTexture.Get());
    g_resources.outputVkFormat = to_vk_format(colorFormat);
    g_resources.mvVkFormat = VK_FORMAT_R16G16_SFLOAT;
    g_resources.outputImageView = create_color_image_view(g_resources.outputImage, g_resources.outputVkFormat);
    g_resources.mvImageView = create_color_image_view(g_resources.mvImage, g_resources.mvVkFormat);
    g_resources.width = width;
    g_resources.height = height;
    g_resources.colorFormat = colorFormat;

    if (!g_resources.matches(width, height, colorFormat)) {
        DuskLog.warn("dlss: failed to create DLAA image views (color format {})", static_cast<int>(colorFormat));
        release_resources();
        return false;
    }

    // A resource change invalidates the temporal history.
    g_reset = true;
    DuskLog.info("dlss: DLAA evaluate resources ready {}x{} (scene color vk format {})", width, height,
                 static_cast<int>(colorVk));
    return true;
}

// Present-path hook: evaluated on the render worker after the scene submission.
aurora::gfx::PostProcessTarget scene_post_process_hook(void* /*userdata*/, const wgpu::Texture& sceneColorTexture,
                                                       const wgpu::TextureView& sceneColorView,
                                                       const wgpu::Texture& sceneDepthTexture,
                                                       const wgpu::TextureView& sceneDepthView, uint32_t width,
                                                       uint32_t height) {
    std::lock_guard<std::mutex> lock(g_dlaaMutex);
    if (g_feature == nullptr || g_params == nullptr || width == 0 || height == 0) {
        return {};
    }
    if (!sceneColorTexture || !sceneColorView || !sceneDepthTexture || !sceneDepthView) {
        return {};
    }
    if (!g_cmd.ensure_ready() || g_cmd.device == VK_NULL_HANDLE || g_cmd.deviceWaitIdle == nullptr) {
        return {};
    }

    // Prototype synchronization: wait for the scene submission on Dawn's queue
    // to finish so the color/depth images are GPU-ready to be read by NGX.
    g_cmd.deviceWaitIdle(g_cmd.device);

    const wgpu::TextureFormat colorFormat = sceneColorTexture.GetFormat();
    if (!ensure_resources(width, height, colorFormat)) {
        return {};
    }

    const VkFormat colorVk = to_vk_format(colorFormat);
    const VkFormat depthVk = to_vk_format(sceneDepthTexture.GetFormat());
    const VkImage colorImage = dawn::native::vulkan::GetVkImage(sceneColorTexture.Get());
    const VkImageView colorView = dawn::native::vulkan::GetVkImageView(sceneColorView.Get());
    const VkImage depthImage = dawn::native::vulkan::GetVkImage(sceneDepthTexture.Get());
    const VkImageView depthView = dawn::native::vulkan::GetVkImageView(sceneDepthView.Get());
    if (colorImage == VK_NULL_HANDLE || colorView == VK_NULL_HANDLE || depthImage == VK_NULL_HANDLE ||
        depthView == VK_NULL_HANDLE || colorVk == VK_FORMAT_UNDEFINED || depthVk == VK_FORMAT_UNDEFINED) {
        return {};
    }

    // --- Camera-reprojection motion vectors (the §7.1 MVP). ---
    //
    // The scene color/depth are GPU-complete (deviceWaitIdle above). Build the
    // reprojection constant C = M_eff_prev * inverse(M_eff_curr) from the
    // current and previous effective world->clip matrices, render motion
    // vectors into the RG16F target, and sync before NGX consumes them.
    const view_class* view = dComIfGd_getView();
    const Mtx44* projViewMtx = (view != nullptr) ? &view->projViewMtx : nullptr;
    Mat4 effCur;
    if (projViewMtx != nullptr) {
        mat4_effective_projview(*projViewMtx, effCur);
    } else {
        mat4_identity(effCur);
    }

    const bool msaa = sceneDepthTexture.GetSampleCount() > 1;
    bool reprojectValid = false;
    Mat4 C;
    if (!msaa && projViewMtx != nullptr && g_mv.hasPrev) {
        Mat4 invCur;
        if (mat4_inverse(effCur, invCur)) {
            mat4_mul(g_mv.prevEff, invCur, C);
            // A wildly different matrix is a camera cut/teleport, not real
            // motion; fall back to a reset instead of reprojecting garbage.
            reprojectValid = mat4_identity_delta(C) < kMvCutThreshold;
        }
    }
    if (!reprojectValid) {
        // First frame / camera cut / no usable history: identity yields zero
        // motion vectors, paired with InReset=1 so NGX discards its history.
        mat4_identity(C);
    }

    bool mvSubmitted = false;
    if (!msaa) {
        mvSubmitted = run_mv_pass(sceneDepthView, C, width, height);
        // The reprojection writes land on Dawn's queue; make them visible to the
        // NGX evaluate on the reserved app queue.
        g_cmd.deviceWaitIdle(g_cmd.device);
    }
    if (projViewMtx != nullptr) {
        std::memcpy(g_mv.prevEff, effCur, sizeof(effCur));
        g_mv.hasPrev = !msaa;
    } else {
        g_mv.hasPrev = false;
    }

    const bool reprojectionActive = reprojectValid && mvSubmitted;

    VkImageSubresourceRange colorRange{};
    colorRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    colorRange.levelCount = 1;
    colorRange.layerCount = 1;
    VkImageSubresourceRange depthRange{};
    depthRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthRange.levelCount = 1;
    depthRange.layerCount = 1;

    NVSDK_NGX_Resource_VK colorRes = NVSDK_NGX_Create_ImageView_Resource_VK(colorView, colorImage, colorRange, colorVk,
                                                                            width, height, false);
    NVSDK_NGX_Resource_VK depthRes =
        NVSDK_NGX_Create_ImageView_Resource_VK(depthView, depthImage, depthRange, depthVk, width, height, false);
    NVSDK_NGX_Resource_VK mvRes = NVSDK_NGX_Create_ImageView_Resource_VK(
        g_resources.mvImageView, g_resources.mvImage, colorRange, g_resources.mvVkFormat, width, height, false);
    NVSDK_NGX_Resource_VK outRes = NVSDK_NGX_Create_ImageView_Resource_VK(
        g_resources.outputImageView, g_resources.outputImage, colorRange, g_resources.outputVkFormat, width, height,
        true);

    const bool reset = g_reset || !reprojectionActive;
    const float jx = g_jitterX.load(std::memory_order_relaxed);
    const float jy = g_jitterY.load(std::memory_order_relaxed);

    // Mirror the fields the SDK's NGX_VULKAN_EVALUATE_DLSS_EXT helper writes,
    // through the NVSDK_NGX_Parameter virtual interface (a single call path that
    // does not depend on additional loader-stub symbols).
    g_params->Set(NVSDK_NGX_Parameter_Color, static_cast<void*>(&colorRes));
    g_params->Set(NVSDK_NGX_Parameter_Output, static_cast<void*>(&outRes));
    g_params->Set(NVSDK_NGX_Parameter_Depth, static_cast<void*>(&depthRes));
    g_params->Set(NVSDK_NGX_Parameter_MotionVectors, static_cast<void*>(&mvRes));
    g_params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X, jx);
    g_params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y, jy);
    g_params->Set(NVSDK_NGX_Parameter_Reset, reset ? 1 : 0);
    g_params->Set(NVSDK_NGX_Parameter_MV_Scale_X, 1.0f);
    g_params->Set(NVSDK_NGX_Parameter_MV_Scale_Y, 1.0f);
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, width);
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, height);
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);

    if (!g_cmd.begin()) {
        return {};
    }

    const NVSDK_NGX_Result evalResult = g_ngx.evaluateFeature(g_cmd.cmdBuffer, g_feature, g_params, nullptr);
    const bool submitted = g_cmd.end_and_submit();

    if (!g_evalLogged) {
        g_evalLogged = true;
        DuskLog.info("DLAA: evaluating {}x{} (result 0x{:08X}, submitted={})", width, height,
                     static_cast<uint32_t>(evalResult), submitted);
    } else if (evalResult != NVSDK_NGX_Result_Success && g_evalFailCount < 8) {
        ++g_evalFailCount;
        DuskLog.warn("DLAA: evaluate failed (0x{:08X}, submitted={})", static_cast<uint32_t>(evalResult), submitted);
    }

    if (evalResult != NVSDK_NGX_Result_Success || !submitted) {
        return {};
    }

    g_reset = false;
    return aurora::gfx::PostProcessTarget{
        .texture = g_resources.outputTexture,
        .view = g_resources.outputView,
        .format = colorFormat,
        .width = width,
        .height = height,
    };
}

// Requires g_dlaaMutex held. Releases the NGX feature handle + evaluate
// resources and stops feeding projection jitter. The command pool is kept (it is
// reused by a later create()/evaluate); it is torn down in shutdown().
void release_feature_locked() {
    if (g_feature != nullptr && g_ngx.releaseFeature != nullptr) {
        g_ngx.releaseFeature(g_feature);
    }
    g_feature = nullptr;
    release_resources();
    g_jitterX.store(0.f, std::memory_order_relaxed);
    g_jitterY.store(0.f, std::memory_order_relaxed);
    g_jitterFrame = 0;
    aurora::gfx::set_projection_jitter(0.f, 0.f);
}

}  // namespace

bool is_supported() {
    return evaluate_support();
}

const char* unavailable_reason() {
    evaluate_support();
    return g_reason;
}

bool initialize() {
    if (!is_supported()) {
        return false;
    }
    if (g_initialized) {
        return true;
    }

    void* vkInstance = call_interop(aurora_dlss_dawn_vk_instance);
    void* vkPhysicalDevice = call_interop(aurora_dlss_dawn_vk_physical_device);
    void* vkDevice = call_interop(aurora_dlss_dawn_vk_device);
    void* getInstanceProcAddr = call_interop(aurora_dlss_dawn_vk_get_instance_proc_addr);
    void* getDeviceProcAddr = call_interop(aurora_dlss_dawn_vk_get_device_proc_addr);
    if (vkInstance == nullptr || vkPhysicalDevice == nullptr || vkDevice == nullptr) {
        DuskLog.warn("dlss: Aurora NGX interop did not provide Vulkan handles");
        return false;
    }

    // NGX needs a writable directory for logs/temp files.
    std::filesystem::path appDataPath = dusk::data::cache_path();
    if (appDataPath.empty()) {
        appDataPath = std::filesystem::temp_directory_path();
    }
    std::error_code ec;
    std::filesystem::create_directories(appDataPath, ec);
    const std::wstring appDataWide = appDataPath.wstring();

    const NVSDK_NGX_Result initResult = g_ngx.init(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, appDataWide.c_str(),
        reinterpret_cast<VkInstance>(vkInstance), reinterpret_cast<VkPhysicalDevice>(vkPhysicalDevice),
        reinterpret_cast<VkDevice>(vkDevice), reinterpret_cast<PFN_vkGetInstanceProcAddr>(getInstanceProcAddr),
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(getDeviceProcAddr), nullptr, NVSDK_NGX_Version_API);
    if (initResult != NVSDK_NGX_Result_Success) {
        DuskLog.warn("dlss: NVSDK_NGX_VULKAN_Init_with_ProjectID failed (0x{:08X})",
                     static_cast<uint32_t>(initResult));
        return false;
    }

    NVSDK_NGX_Parameter* params = nullptr;
    if (g_ngx.getCapabilityParameters(&params) != NVSDK_NGX_Result_Success || params == nullptr) {
        DuskLog.warn("dlss: NVSDK_NGX_VULKAN_GetCapabilityParameters failed");
        g_ngx.shutdown(reinterpret_cast<VkDevice>(vkDevice));
        return false;
    }

    int superSamplingAvailable = 0;
    params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &superSamplingAvailable);
    DuskLog.info("dlss: NVSDK_NGX_VULKAN_Init_with_ProjectID succeeded; "
                 "SuperSampling_Available={}",
                 superSamplingAvailable);
    if (superSamplingAvailable == 0) {
        DuskLog.warn("dlss: SuperSampling is not available on this system");
        g_ngx.destroyParameters(params);
        g_ngx.shutdown(reinterpret_cast<VkDevice>(vkDevice));
        return false;
    }

    g_params = params;
    g_vkDevice = vkDevice;
    g_initialized = true;
    // Install the present-path hook that evaluates DLAA on the finished scene
    // and hands Aurora its output to blit. Inert (returns an empty target)
    // until a feature is created.
    aurora::gfx::set_scene_post_process(&scene_post_process_hook, nullptr);
    DuskLog.info("dlss: NGX initialized (DLAA present hook registered)");
    return true;
}

void shutdown() {
    // Stop the present hook first so the render worker cannot start a new
    // evaluate while NGX is being torn down.
    aurora::gfx::set_scene_post_process(nullptr, nullptr);
    {
        std::lock_guard<std::mutex> lock(g_dlaaMutex);
        // Drain the GPU before releasing the feature/views so nothing is using
        // them when NGX and the driver tear down.
        if (g_cmd.deviceWaitIdle != nullptr && g_cmd.device != VK_NULL_HANDLE) {
            g_cmd.deviceWaitIdle(g_cmd.device);
        }
        release_feature_locked();
        release_resources();
        // Drop the device-owned motion-vector pipeline objects.
        g_mv.pipeline = {};
        g_mv.layout = {};
        g_mv.uniform = {};
    }
    g_cmd.shutdown();
    if (g_params != nullptr && g_ngx.destroyParameters != nullptr) {
        g_ngx.destroyParameters(g_params);
    }
    g_params = nullptr;
    if (g_initialized && g_ngx.shutdown != nullptr) {
        g_ngx.shutdown(reinterpret_cast<VkDevice>(g_vkDevice));
    }
    g_initialized = false;
    g_vkDevice = nullptr;
}

bool create(uint32_t renderWidth, uint32_t renderHeight, Quality quality) {
    if (!is_supported() || !g_initialized || g_params == nullptr) {
        return false;
    }
    if (renderWidth == 0 || renderHeight == 0 || quality != Quality::Dlaa) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_dlaaMutex);
    if (g_feature != nullptr) {
        release_feature_locked();
    }

    // Equivalent of the SDK's NGX_VULKAN_CREATE_DLSS_EXT1 helper, but with the
    // parameters written through the NVSDK_NGX_Parameter virtual interface
    // (which lives in the NGX runtime) rather than the SDK's static helper
    // functions, which would require linking libnvsdk_ngx.a.
    g_params->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    g_params->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    g_params->Set(NVSDK_NGX_Parameter_Width, renderWidth);
    g_params->Set(NVSDK_NGX_Parameter_Height, renderHeight);
    g_params->Set(NVSDK_NGX_Parameter_OutWidth, renderWidth);
    g_params->Set(NVSDK_NGX_Parameter_OutHeight, renderHeight);
    g_params->Set(NVSDK_NGX_Parameter_PerfQualityValue, static_cast<int>(NVSDK_NGX_PerfQuality_Value_DLAA));
    // Aurora renders with reversed-Z depth. Motion vectors are produced at
    // render resolution by a camera-reprojection pass (MVLowRes); they are not
    // jittered because that pass uses the unjittered camera matrix.
    const int createFlags = static_cast<int>(NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                                             NVSDK_NGX_DLSS_Feature_Flags_MVLowRes);
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, createFlags);
    g_params->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);

    // NGX requires an *open and recording* command buffer here; passing
    // VK_NULL_HANDLE makes it dereference a null command buffer and crash
    // inside libnvidia-glcore.
    if (!g_cmd.begin()) {
        DuskLog.warn("dlss: could not begin Vulkan command buffer for DLAA feature creation");
        return false;
    }

    NVSDK_NGX_Handle* handle = nullptr;
    const NVSDK_NGX_Result createResult = g_ngx.createFeature(reinterpret_cast<VkDevice>(g_vkDevice),
                                                              g_cmd.cmdBuffer,
                                                              NVSDK_NGX_Feature_SuperSampling, g_params, &handle);
    const bool submitted = g_cmd.end_and_submit();

    if (createResult != NVSDK_NGX_Result_Success || handle == nullptr || !submitted) {
        DuskLog.warn("dlss: DLAA feature creation failed (0x{:08X}, submitted={})",
                     static_cast<uint32_t>(createResult), submitted);
        return false;
    }

    g_feature = handle;
    g_reset = true;
    DuskLog.info("DLAA: feature created {}x{}", renderWidth, renderHeight);
    return true;
}

void release() {
    std::lock_guard<std::mutex> lock(g_dlaaMutex);
    release_feature_locked();
}

void set_reset(bool reset) {
    g_reset = reset;
}

void advance_jitter() {
    if (!is_supported() || g_feature == nullptr) {
        return;
    }
    float jx = 0.f;
    float jy = 0.f;
    get_jitter(g_jitterFrame++, &jx, &jy);
    g_jitterX.store(jx, std::memory_order_relaxed);
    g_jitterY.store(jy, std::memory_order_relaxed);
    aurora::gfx::set_projection_jitter(jx, jy);
}

#endif  // DUSK_ENABLE_DLSS

}  // namespace dusk::dlss
