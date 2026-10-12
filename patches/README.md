# DLAA / NGX patches

These patches are the out-of-tree changes required for Dusklight's NVIDIA
DLAA support (`DUSK_ENABLE_DLSS`). Everything else lives in the normal
`src/` tree and is on this branch.

> **Licensing:** the NVIDIA DLSS SDK is proprietary and is **not** committed.
> `tools/fetch_dlss_sdk.sh` downloads it into `build/dlss_sdk/` (gitignored).

## Contents

| File | Applies to | What it adds |
|---|---|---|
| `dawn-ngx-interop.patch` | `encounter/dawn` @ `1155e0ed531126f33a1279afa029349651ca1c93` | Exported Vulkan handle getters (`GetVkDevice`/`GetVkPhysicalDevice`/`GetQueueFamilyIndex`/`GetDeviceProcAddr`/`GetAppVkQueue`), texture getters (`GetVkImage`/`GetVkImageView`), app-provided extra instance/device extensions appended at `vkCreateInstance`/`vkCreateDevice`, and an optional second app-reserved `VkQueue`. Default behavior is unchanged when nothing is requested. |
| `aurora-dlaa-hook.patch` | `encounter/aurora` @ `3227d76c60e1e782ca576610bce61c9e7744d8be` | `aurora::gfx::set_scene_post_process()` — a present-path hook that finishes+submits the scene, then blits a hook-returned target to the swapchain; `aurora::gfx::set_projection_jitter()` — perspective-only sub-pixel projection jitter injected at the GX uniform choke point; an optional per-object motion-vector scene attachment (RG16F) exposed to the hook; and a `GXSetScreenSpaceOverlay()` state (GX_AURORA subcommand `0x0044`) that excludes full-screen screen-space overlays (fade veils, framebuffer-snapshot transitions, screen filters) from the motion-vector bookkeeping — they no longer write the MV attachment, update the frame's recorded perspective projection, or receive projection jitter. |

> The per-object motion-vector path also requires the parent-repo change
> `libs/JSystem/src/J3DGraphBase/J3DGD.cpp` (routes `J3DFifoLoadPosMtxImm`
> through `GXLoadPosMtxImm` so the source pointer is visible); it is already in
> this branch's commits.

## 1. Dawn

The patched Dawn is consumed through Aurora's **`package`** provider. The
default source is the prebuilt tarball published as the `dawn-ngx-1`
`--prerelease` asset on the fork
(`https://github.com/vpkoskinen/dusklight/releases/download/dawn-ngx-1/dawn-linux-x86_64.tar.gz`),
which the `linux-dlss` preset (see §3) points `AURORA_DAWN_PACKAGE_URL` at; no
Dawn fork needs to be published. The steps below are how to (re)build that
package from source if you want a local artifact.

```sh
# a) get the pinned Dawn source
mkdir -p build/dawn-ngx/src
curl -sL https://github.com/encounter/dawn/archive/1155e0ed531126f33a1279afa029349651ca1c93.tar.gz \
  | tar -xz -C build/dawn-ngx/src --strip-components=1

# b) apply the patch
patch -p1 -d build/dawn-ngx/src < patches/dawn-ngx-interop.patch

# c) configure/build/install exactly as Dawn's own release CI does
cd build/dawn-ngx/src
cmake -S . -B ../build -G Ninja -C .github/workflows/dawn-ci.cmake \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/../install
cmake --build ../build -j"$(nproc)"
cmake --install ../build
cd -

# d) package it as an install tree
tar -czf build/dawn-ngx/dawn-linux-x86_64.tar.gz -C build/dawn-ngx/install .
```

The package needs a C++ toolchain with libstdc++ (the release CI uses clang;
the default Dusklight preset uses GCC — both are libstdc++, so ABI-compatible).

## 2. Aurora

Aurora is a **git submodule pinned at the upstream commit**
`3227d76c60e1e782ca576610bce61c9e7744d8be` (`encounter/aurora`) — there is no
local-only fork of the submodule. The DLAA changes are delivered purely as the
tracked patch `aurora-dlaa-hook.patch`, which is a `git diff` of the upstream
pin against the patched tree.

For `DUSK_ENABLE_DLSS=ON` builds CMake **applies the patch automatically at
configure time** (see `cmake/PatchAurora.cmake`, invoked from the top-level
`CMakeLists.txt`); the submodule then shows as dirty (`git submodule status`
prints `+`/`-`, and `git status` reports "modified content") — that is expected
and intended. The default build (`DUSK_ENABLE_DLSS=OFF`) never touches the
submodule and works against a pristine checkout.

To apply it by hand instead:

```sh
git -C extern/aurora checkout 3227d76c60e1e782ca576610bce61c9e7744d8be
patch -p1 -d extern/aurora < patches/aurora-dlaa-hook.patch
```

To re-roll the patch after editing the submodule:

```sh
git -C extern/aurora diff 3227d76c60e1e782ca576610bce61c9e7744d8be \
  > patches/aurora-dlaa-hook.patch
```

CMake skips the patch if it is already applied (a reverse-apply check
succeeds), so re-running configure is safe. `-DDUSK_AURORA_PATCH=OFF` skips it
entirely (for debugging); a `DUSK_ENABLE_DLSS` build will then fail to compile
unless the patch was applied by hand.

The patch adds the `GX_AURORA` subcommand
`GX_AURORA_SET_SCREEN_SPACE_OVERLAY` (`0x0044`) and the `GXSetScreenSpaceOverlay()`
API, and bumps `GXPipelineConfigVersion` (13 → 14) because `PipelineConfig`
gained a `flags` field. The Dusk-side call sites are all gated on
`#if TARGET_PC && DUSK_ENABLE_DLSS`, so the default build does not reference
`GXSetScreenSpaceOverlay` at all.


## 3. Build & run Dusklight with DLAA

The `linux-dlss` CMake preset wires everything up:

```sh
./tools/fetch_dlss_sdk.sh          # -> build/dlss_sdk (headers + loader stub)

cmake --preset linux-dlss          # fetches the patched Dawn package (see below)
cmake --build build/linux-dlss -j"$(nproc)"
```

Configure needs network access: the preset downloads the patched prebuilt Dawn
package from the release asset
`https://github.com/vpkoskinen/dusklight/releases/download/dawn-ngx-1/dawn-linux-x86_64.tar.gz`
(a `--prerelease` build artifact on the fork — **not** a Dusklight release; see
the release notes for its sha256, Dawn commit and license). This is the
equivalent of the previous hand-written
`-DDUSK_ENABLE_DLSS=ON -DDUSK_DLSS_SDK_DIR=… -DAURORA_DAWN_PROVIDER=package
-DAURORA_DAWN_PACKAGE_URL=…` invocation.

To consume a **locally built** package instead (e.g. the one produced by §1),
override the URL:

```sh
cmake --preset linux-dlss \
  -DAURORA_DAWN_PACKAGE_URL="file://$PWD/build/dawn-ngx/dawn-linux-x86_64.tar.gz"
```

NGX finds the DLSS runtime snippet (`libnvidia-ngx-dlss.so`) by scanning the
**executable's directory**, not `LD_LIBRARY_PATH`. For local testing, symlink the
SDK's snippet next to the binary:

```sh
ln -sf "$PWD/build/dlss_sdk/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.1" \
       build/linux-dlss/libnvidia-ngx-dlss.so

build/linux-dlss/dusklight --backend vulkan --dvd /path/to/game.rvz
```

DLAA appears as **Anti-Aliasing → DLAA** in the Video settings (enabled only
when NGX is available). The default build (`DUSK_ENABLE_DLSS=OFF`) is unaffected
and has zero NVIDIA dependency.

## Known limitations (MVP)

- Motion vectors are camera-reprojection only (moving objects reprojected as
  static) — see `PLAN.md` §10 Phase E.
- Cross-queue synchronization is prototype `vkDeviceWaitIdle`.
- The bridge links the SDK loader stub; a shippable build should use the
  dynamic loader path and ship only the runtime snippet + NVIDIA's license.
