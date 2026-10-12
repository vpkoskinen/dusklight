# AGENTS.md — Guide for Working on Dusklight

## Project Overview

**Dusklight** is a reverse-engineered reimplementation of *The Legend of Zelda:
Twilight Princess*. It aims to be as accurate to the original as possible while
providing new options, enhancements, and modding tools. It is based on the
[zeldaret/tp decompilation](https://github.com/zeldaret/tp).

- Official website: <https://twilitrealm.dev/>
- Discord: <https://discord.gg/6NpMhefCK9>
- Built with the [Aurora](https://github.com/encounter/aurora) engine.

> **Important:** Dusklight does **not** ship any copyrighted game assets.
> Users must dump their own copy of the game first. See `README.md` and
> `docs/building.md` for full setup instructions.

---

## Build System

The project is built with **CMake** (3.25+). The entry point is the
`build.sh` script at the project root:

```sh
./build.sh
```

This runs:

```sh
cmake --preset linux-default-relwithdebinfo
cmake --build --preset linux-default-relwithdebinfo
```

`build.sh` is **not tracked by git** (it is listed in `.gitignore` via
`build/` and generated files). Do not commit changes to it unless you
intend to alter the default build preset.

### Available CMake Presets

See `docs/building.md` for full details. Quick reference:

| Preset | Platform / Compiler | Build Type |
|---|---|---|
| `linux-default-relwithdebinfo` | GCC | RelWithDebInfo |
| `linux-default-debug` | GCC | Debug |
| `linux-default-debug-asan` | GCC | Debug + ASan |
| `linux-clang-relwithdebinfo` | Clang | RelWithDebInfo |
| `linux-clang-debug` | Clang | Debug |
| `linux-clang-debug-asan` | Clang | Debug + ASan |
| `windows-msvc-relwithdebinfo` | MSVC | RelWithDebInfo |
| `windows-msvc-debug` | MSVC | Debug |
| `windows-msvc-debug-asan` | MSVC | Debug + ASan |
| `windows-clang-relwithdebinfo` | Clang-cl | RelWithDebInfo |
| `windows-clang-debug` | Clang-cl | Debug |
| `macos-default-relwithdebinfo` | Clang | RelWithDebInfo |
| `macos-default-debug` | Clang | Debug |
| `macos-default-debug-asan` | Clang | Debug + ASan |

Run with: `cmake --preset <preset-name>` then `cmake --build --preset <preset-name>`.

### Running the Build

```sh
build/{preset}/dusklight --dvd /path/to/game.iso
```

macOS produces an `.app` bundle:
```sh
build/{preset}/Dusklight.app/Contents/MacOS/Dusklight --dvd /path/to/game.iso
```

Supported disc image formats: ISO (GCM), RVZ, WIA, WBFS, CISO, GCZ.

### DLAA / DLSS (experimental, default OFF)

NVIDIA DLAA (NGX) lives behind `DUSK_ENABLE_DLSS` (**default OFF**; the default
build has no NVIDIA dependency and needs **no** patching — a pristine
`extern/aurora` checkout builds it). It needs two out-of-tree layers, both
tracked in `patches/` (+ `patches/README.md`): a patched **Aurora** (delivered
as `aurora-dlaa-hook.patch`, applied automatically by CMake when
`DUSK_ENABLE_DLSS=ON`) and a patched **prebuilt Dawn** package, fetched from the
fork's `dawn-ngx-1` `--prerelease` release asset (a build artifact, not a
Dusklight release). Configure therefore needs network access; override
`AURORA_DAWN_PACKAGE_URL` with a `file://…` URL to use a local package.

```sh
./tools/fetch_dlss_sdk.sh                 # SDK headers + loader stub -> build/dlss_sdk
cmake --preset linux-dlss
cmake --build build/linux-dlss -j"$(nproc)"
# NGX scans the *executable's* directory for the snippet, not LD_LIBRARY_PATH:
ln -sf "$PWD/build/dlss_sdk/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.1" \
       build/linux-dlss/libnvidia-ngx-dlss.so
build/linux-dlss/dusklight --backend vulkan <game.rvz>   # Video -> Anti-Aliasing -> DLAA
```

Gotchas: link the SDK's `libnvsdk_ngx.a` loader stub instead of `dlsym`-ing the
runtime; `CreateFeature1` needs a live recording command buffer; screen-space
overlays (fade veils, screen filters) must be wrapped in
`GXSetScreenSpaceOverlay()` or they corrupt DLAA motion vectors; Aurora's
`PipelineConfig` must stay padding-free (`has_unique_object_representations`
static_assert) and `GXPipelineConfigVersion` must be bumped when it changes.

---

## Git Workflow

This repository is a **fork** of `TwilitRealm/dusklight`, and the remotes follow
the standard fork convention:

| Remote | Repository | Use |
|---|---|---|
| `origin` | `vpkoskinen/dusklight` — **your fork** | Push all work here. `gh` targets this repo by default. |
| `upstream` | `TwilitRealm/dusklight` | **Read-only.** Fetch it to keep `main` current; never push. |

`gh` is pinned to the fork (`git config remote.origin.gh-resolved=base`, set via
`gh repo set-default vpkoskinen/dusklight`), so `gh release`, `gh run`, `gh pr`,
etc. operate on your fork. Pass `-R TwilitRealm/dusklight` when you deliberately
want to act on upstream (e.g. opening an upstream PR). The fork's default branch
is `custom`, so a plain `git clone --recursive` of the fork builds your work.

### Branching Model

| Branch | Purpose |
|---|---|
| `main` | Vanilla upstream copy, kept in sync with `upstream/main` (fast-forward only). |
| `custom` | Your integration branch; pushed to `origin/custom` and the fork's default branch. |
| `feature/<name>` | Individual features, developed on their own branch before merging. |

### Rules

1. **Never push to `upstream`** — that repository is not ours. Pushes to
   `origin` (your fork) are expected; pushing `main` there is fine too.

2. **`main` stays vanilla.** Only ever fast-forward it to `upstream/main`; never
   commit to it directly.

3. **`custom` is your integration branch.** Each feature is developed on its own
   branch first (see below), then merged into `custom` when ready and pushed.

4. **Feature branches** (`feature/<name>`) are created, worked on, and tested
   independently before being merged into `custom`.

5. **Rebasing `custom` onto `main`:** only on explicit user request; never
   auto-rebase. `custom` is published, so a rebase needs
   `git push --force-with-lease origin custom`.

### Typical Workflow

```sh
# Keep your main current with upstream (fast-forward only)
git fetch upstream
git checkout main
git merge --ff-only upstream/main
git push origin main

# Create a new feature branch from custom
git checkout custom
git checkout -b feature/my-feature

# ... develop, commit on the feature branch ...

# Merge into custom and publish it to your fork
git checkout custom
git merge feature/my-feature
git push origin custom

# Periodically (on user request) rebase custom onto main
git checkout custom
git rebase main
git push --force-with-lease origin custom
```

---

## Code Conventions

For details, see `docs/code-conventions.md`. Key points:

### Upstream When Appropriate

Bug fixes, documentation improvements, code cleanup, or changes that also apply
to the original [zeldaret/tp decompilation](https://github.com/zeldaret/tp)
should preferably be PR'd there first, not just to Dusklight.

### Delineate Dusk-Specific Code

When modifying original game code (i.e. code shared with the decompilation
project) for Dusk's purposes, clearly mark it as Dusk-specific:

- Use `#if TARGET_PC` to guard Dusk-specific changes, keeping the original
  code in the `#else` branch.
- Use `#if AVOID_UB` for Undefined Behavior fixes to the original codebase.

### Memory Allocation

The original codebase uses global `operator new` overloads to allocate into a
strict tree of heaps. This causes linkage issues, so effectively all `new` /
`delete` in original game code has been replaced with `JKR_NEW`, `JKR_DELETE`,
or similar macros. See `JKRHeap.h` for the full list.

---

## Project Structure

| Directory | Contents |
|---|---|
| `src/` | Source code (`dusk/`, game code, JSystem libs) |
| `include/` | Header files |
| `mods/` | In-tree code mods (template, randomizer, cosmetics, etc.) |
| `extern/` | External dependencies (Aurora, Borealis) |
| `libs/` | Additional libraries (freeverb) |
| `res/` | Resources (icons, assets) |
| `platforms/` | Platform-specific code (iOS, tvOS, macOS, Windows) |
| `cmake/` | CMake modules and scripts |
| `docs/` | Documentation (building, code conventions, modding) |
| `ci/` | CI scripts (e.g. AppImage build) |
| `tools/` | Build and utility tools |
| `sdk/` | SDK headers |

---

## Additional Notes

- **CMake Variants:** `cmake-variants.yaml` and `CMakePresets.json` provide
  IDE integration presets. CLion and VS Code can load these automatically.
- **Submodules:** Initialize with `git submodule update --init --recursive`.
  The Aurora gitlink is pinned at upstream; the DLAA changes are applied by
  CMake from `patches/aurora-dlaa-hook.patch` **only** for `DUSK_ENABLE_DLSS`
  builds (see `patches/README.md`). Default builds use a pristine submodule.
- **Local notes:** `CONTEXT.md` (live handoff) and `PLAN.md` (feature plan) are
  untracked, gitignored agent notes — keep them out of commits and out of
  `custom`/upstream.
- **Nix:** A `flake.nix` / `flake.lock` are provided for Nix-based development.
- **Formatting:** `.clang-format`, `.flake8` exist for code styling.
- **PCH:** The project uses a precompiled header (`include/dusk_pch.hpp`).
- **Modding:** Code mods are enabled via `DUSK_ENABLE_CODE_MODS` (on by
  default). See `docs/modding.md` for mod development guidance.

---

## Quick Reference

| Task | Command |
|---|---|
| Build | `./build.sh` |
| Configure (Linux) | `cmake --preset linux-default-relwithdebinfo` |
| Build (all presets) | `cmake --build --preset <preset-name>` |
| Sync `main` with upstream | `git fetch upstream && git checkout main && git merge --ff-only upstream/main` |
| Publish work | `git push origin custom` |
| Rebase custom onto main | `git checkout custom && git rebase main` then `git push --force-with-lease origin custom` |

For full build and run instructions, see `docs/building.md`.
For modding, see `docs/modding.md`.
