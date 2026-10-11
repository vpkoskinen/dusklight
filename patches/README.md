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
| `aurora-dlaa-hook.patch` | `encounter/aurora` @ `3227d76c60e1e782ca576610bce61c9e7744d8be` | `aurora::gfx::set_scene_post_process()` — a present-path hook that finishes+submits the scene, then blits a hook-returned target to the swapchain; `aurora::gfx::set_projection_jitter()` — perspective-only sub-pixel projection jitter injected at the GX uniform choke point; and an optional per-object motion-vector scene attachment (RG16F) exposed to the hook. |

> The per-object motion-vector path also requires the parent-repo change
> `libs/JSystem/src/J3DGraphBase/J3DGD.cpp` (routes `J3DFifoLoadPosMtxImm`
> through `GXLoadPosMtxImm` so the source pointer is visible); it is already in
> this branch's commits.

## 1. Dawn

The patched Dawn is consumed through Aurora's **`package`** provider pointing at
a local prebuilt tarball — no Dawn fork needs to be published.

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

This branch has Aurora as a **git submodule** with a **local-only** commit
(`592d908`) that is not on any remote (we must not push to `encounter/aurora`).
Two ways to reproduce:

* Point the submodule at the local commit (already the case on this branch), or
* Apply the patch onto the base commit:
  ```sh
  git -C extern/aurora checkout 3227d76c60e1e782ca576610bce61c9e7744d8be
  patch -p1 -d extern/aurora < patches/aurora-dlaa-hook.patch
  ```

## 3. Build & run Dusklight with DLAA

```sh
./tools/fetch_dlss_sdk.sh                       # -> build/dlss_sdk (headers + loader stub)

cmake -S . -B build/linux-dlss -G Ninja \
  -DDUSK_ENABLE_DLSS=ON \
  -DDUSK_DLSS_SDK_DIR="$PWD/build/dlss_sdk" \
  -DAURORA_DAWN_PROVIDER=package \
  -DAURORA_DAWN_PACKAGE_URL="file://$PWD/build/dawn-ngx/dawn-linux-x86_64.tar.gz"
cmake --build build/linux-dlss -j"$(nproc)"
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
