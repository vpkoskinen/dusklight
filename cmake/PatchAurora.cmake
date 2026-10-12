# Applies patches/aurora-dlaa-hook.patch to the extern/aurora submodule.
#
# The DLAA/DLSS feature needs Aurora APIs (present-path hook, projection jitter,
# per-object motion vectors, screen-space-overlay state) that are NOT part of
# the upstream Aurora pin. Instead of carrying a local-only fork of the
# submodule, Dusklight keeps extern/aurora at the upstream commit and applies
# the tracked patch at configure time.
#
# This only runs for DUSK_ENABLE_DLSS builds: the default build works against a
# pristine submodule and needs no patching. Afterwards the submodule is expected
# to show up as dirty ("modified content") in `git submodule status`; that is
# intended. Re-running CMake is safe: if the patch is already applied, this is a
# no-op.
#
# Escape hatch: set -DDUSK_AURORA_PATCH=OFF to skip patching (e.g. for
# debugging). A DUSK_ENABLE_DLSS build will then fail to compile unless the
# patch was applied by hand.

function(dusk_apply_aurora_patch)
    set(_aurora_pin "3227d76c60e1e782ca576610bce61c9e7744d8be")
    set(_aurora_dir "${CMAKE_SOURCE_DIR}/extern/aurora")
    set(_patch_file "${CMAKE_SOURCE_DIR}/patches/aurora-dlaa-hook.patch")

    if (NOT EXISTS "${_aurora_dir}/CMakeLists.txt")
        message(FATAL_ERROR
            "extern/aurora is missing or empty.\n"
            "Run: git submodule update --init extern/aurora")
    endif ()

    if (NOT EXISTS "${_patch_file}")
        message(FATAL_ERROR
            "Aurora patch not found: ${_patch_file}\n"
            "Run: git submodule update --init extern/aurora")
    endif ()

    find_program(_git_exe git REQUIRED)

    # Already applied? A successful reverse-check means the tree already matches
    # the patch's post-state, either because the changes are committed (local
    # fork) or present as a dirty working tree.
    execute_process(
        COMMAND "${_git_exe}" -C "${_aurora_dir}" apply --reverse --check "${_patch_file}"
        RESULT_VARIABLE _reverse_result
        OUTPUT_QUIET
        ERROR_QUIET)
    if (_reverse_result EQUAL 0)
        message(STATUS "Aurora patch already applied; skipping")
        return()
    endif ()

    # Verify it applies cleanly before touching the tree.
    execute_process(
        COMMAND "${_git_exe}" -C "${_aurora_dir}" apply --check "${_patch_file}"
        RESULT_VARIABLE _check_result
        OUTPUT_VARIABLE _check_out
        ERROR_VARIABLE _check_err)
    if (NOT _check_result EQUAL 0)
        message(FATAL_ERROR
            "patches/aurora-dlaa-hook.patch does not apply to extern/aurora.\n"
            "${_check_out}${_check_err}"
            "The Aurora pin (${_aurora_pin}) probably moved or the patch is stale.\n"
            "Re-roll it with:\n"
            "  git -C extern/aurora diff ${_aurora_pin} > patches/aurora-dlaa-hook.patch")
    endif ()

    execute_process(
        COMMAND "${_git_exe}" -C "${_aurora_dir}" apply "${_patch_file}"
        RESULT_VARIABLE _apply_result
        OUTPUT_VARIABLE _apply_out
        ERROR_VARIABLE _apply_err)
    if (NOT _apply_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to apply patches/aurora-dlaa-hook.patch to extern/aurora.\n"
            "${_apply_out}${_apply_err}"
            "Re-roll it with:\n"
            "  git -C extern/aurora diff ${_aurora_pin} > patches/aurora-dlaa-hook.patch")
    endif ()

    message(STATUS "Applied patches/aurora-dlaa-hook.patch to extern/aurora "
                   "(the submodule now shows as dirty; this is expected)")
endfunction()
