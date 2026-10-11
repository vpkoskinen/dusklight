#pragma once

#include <cstdint>

// Dusklight-side DLSS/DLAA bridge.
//
// This module is always compiled, but all NVIDIA/NGX-dependent code is gated
// behind the `DUSK_ENABLE_DLSS` build flag (default OFF). When the flag is off
// every entry point below is a safe no-op (except `get_jitter`, which is pure
// math and always works), and no NVIDIA headers are included.
//
// When the flag is on, the bridge dynamically loads the NVIDIA NGX runtime at
// runtime and stays inert unless a Dawn build (or Aurora NGX shim) exports the
// weak `aurora_dlss_dawn_*` Vulkan interop contract. This keeps the default
// build free of any NVIDIA dependency and makes the bridge become functional
// automatically once the required interop is available.

namespace dusk::dlss {

/// DLSS/DLAA quality mode. Only DLAA is exposed today; future DLSS upscaling
/// modes (Quality/Balanced/Performance) will extend this enum.
enum class Quality : int {
    Dlaa = 0,
};

/// Registers the Vulkan instance/device extensions the NVIDIA NGX runtime needs
/// with the graphics backend. Must be called before the render device is
/// created (i.e. before `aurora_initialize`), because Dawn only appends the
/// requested extensions to `vkCreateDevice`/`vkCreateInstance` at creation
/// time. Safe no-op when DLSS support is not compiled in or NGX is unavailable,
/// so non-NVIDIA systems are unaffected.
void register_vulkan_extensions();

/// Returns true only if DLAA can actually run right now: the NGX runtime is
/// present, the Dawn/Aurora Vulkan interop contract is exported, and the active
/// adapter is an NVIDIA GPU on the Vulkan backend.
///
/// Safe to call at any time. The result is cached once it is definitive; while
/// the render device does not exist yet the call returns false without caching
/// so a later call can still succeed.
bool is_supported();

/// Short human-readable reason why `is_supported()` is false. Returns an empty
/// string (never null) when DLAA is supported.
const char* unavailable_reason();

/// One-time NGX initialization. Call once after the render device exists.
/// Returns false (and never crashes) when DLAA is unavailable.
bool initialize();

/// Tears down NGX and releases all resources.
void shutdown();

/// Creates (or recreates) the DLAA feature for the given render resolution.
bool create(uint32_t renderWidth, uint32_t renderHeight, Quality quality);

/// Releases the current DLAA feature (if any).
void release();

/// Marks the next evaluation as a reset (camera cut / first frame).
void set_reset(bool reset);

/// Advances and feeds the DLAA sub-pixel projection jitter for the next frame.
/// Call once per frame while DLAA is active (after each frame is rendered); the
/// value is consumed by both Aurora's projection jitter and the NGX evaluate
/// call. Releases the jitter (and resets the phase) when DLAA is released.
void advance_jitter();

/// Writes the Halton(2,3) sub-pixel jitter offset for `frameIndex`, in
/// render-pixel space (roughly [-0.5, 0.5]). Always available, even when the
/// DLSS build flag is off.
void get_jitter(uint64_t frameIndex, float* outX, float* outY);

}  // namespace dusk::dlss
