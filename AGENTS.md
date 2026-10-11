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

---

## Git Workflow

This repository is a **fork** of `TwilitRealm/dusklight`. The working clone
is **read-only with respect to the upstream remote** — never push directly to
the upstream repository.

### Branching Model

| Branch | Purpose |
|---|---|
| `main` | Vanilla upstream copy. Kept in sync with upstream via fetch only. |
| `custom` | Your personal tweaks and aggregated feature work. |
| `feature/<name>` | Individual features, developed on their own branch before merging. |

### Rules

1. **Never push anything** to the upstream remote. This clone is not owned by
   you. All pushes to `origin` are prohibited.

2. **`main` stays vanilla.** Fetch `origin/main` regularly to check whether
   upstream has updated, but do not commit changes directly to `main`.

3. **`custom` is your integration branch.** Aggregate feature work here.
   Each feature should be developed on its own branch first (see below), then
   merged into `custom` when ready.

4. **Feature branches** (`feature/<name>`) are created, worked on, and tested
   independently before being merged into `custom`.

5. **Rebasing `custom` onto `main`:** Periodically rebase `custom` onto the
   latest `main` to stay in sync with upstream. **Only do this on explicit
   user request.** Do not auto-rebase.

### Typical Workflow

```sh
# Fetch upstream updates (keep main in sync)
git fetch origin

# Create a new feature branch from custom
git checkout custom
git checkout -b feature/my-feature

# ... develop, commit on the feature branch ...

# Merge into custom when ready
git checkout custom
git merge feature/my-feature

# Periodically (on user request) rebase custom onto main
git checkout custom
git rebase main
# Resolve conflicts if any, then continue
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
| Fetch upstream | `git fetch origin` |
| Rebase custom onto main | `git checkout custom && git rebase main` |

For full build and run instructions, see `docs/building.md`.
For modding, see `docs/modding.md`.
