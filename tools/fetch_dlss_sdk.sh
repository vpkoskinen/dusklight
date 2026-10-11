#!/usr/bin/env bash
#
# fetch_dlss_sdk.sh - download the NVIDIA DLSS SDK headers + Linux libraries.
#
# The DLSS SDK is NVIDIA-proprietary: it is NOT redistributable and MUST NOT be
# committed to this repository. It is only needed to build with
# -DDUSK_ENABLE_DLSS=ON; the default build does not reference it at all.
#
# Usage:
#   tools/fetch_dlss_sdk.sh [DEST]
#
# DEST precedence: $1 > $DUSK_DLSS_SDK_DIR > <repo>/build/dlss_sdk.
# The resulting layout is:
#   <DEST>/include/nvsdk_ngx.h
#   <DEST>/lib/Linux_x86_64/...
#
# Then configure with:
#   cmake --preset <preset> -DDUSK_ENABLE_DLSS=ON -DDUSK_DLSS_SDK_DIR=<DEST>
#
# Re-running is a no-op once the SDK is present; pass --force to refresh it.
set -euo pipefail

# --- resolve destination ------------------------------------------------------
repo_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

force=0
if [ "${1:-}" = "--force" ]; then
    force=1
    shift
fi
dest="${1:-${DUSK_DLSS_SDK_DIR:-$repo_root/build/dlss_sdk}}"

# --- resolve source -----------------------------------------------------------
# Preferred: ask the GitHub releases API for the newest SDK and, if an asset
# literally named DLSS_<version>_SDK.zip exists, use it. The official release
# assets are currently ngx_dlss_demo_*.zip (which do NOT contain the SDK
# headers), so when no such asset is present we use the release's source
# archive instead - it ships include/ and lib/Linux_x86_64/.
# Fallback: a documented, pinned source-archive URL used when the API fails.
api_url="https://api.github.com/repos/NVIDIA/DLSS/releases/latest"
pinned_version="310.9.1"
pinned_url="https://github.com/NVIDIA/DLSS/archive/refs/tags/v${pinned_version}.tar.gz"

# --- idempotency --------------------------------------------------------------
if [ "$force" -eq 0 ] && [ -f "$dest/include/nvsdk_ngx.h" ]; then
    echo "NVIDIA DLSS SDK already present at: $dest"
    echo "(pass --force to re-download)"
    exit 0
fi

download() {
    # download <url> <output>
    if command -v curl >/dev/null 2>&1; then
        curl -fL --retry 3 --progress-bar -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then
        wget --show-progress -O "$2" "$1"
    else
        echo "error: neither curl nor wget is available" >&2
        return 1
    fi
}

fetch_api() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$api_url" 2>/dev/null
    elif command -v wget >/dev/null 2>&1; then
        wget -qO- "$api_url" 2>/dev/null
    fi
}

archive_url=""
api_json="$(fetch_api || true)"
if [ -n "${api_json:-}" ]; then
    tag=""
    sdk_asset=""
    if command -v jq >/dev/null 2>&1; then
        tag="$(printf '%s' "$api_json" | jq -r '.tag_name // empty')"
        sdk_asset="$(printf '%s' "$api_json" \
            | jq -r '.assets[]? | select(.name | test("^DLSS_.*_SDK\\.zip$")) | .browser_download_url' \
            | head -n1)"
    else
        tag="$(printf '%s' "$api_json" | sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)"
    fi
    if [ -n "${sdk_asset:-}" ]; then
        archive_url="$sdk_asset"
    elif [ -n "${tag:-}" ]; then
        archive_url="https://github.com/NVIDIA/DLSS/archive/refs/tags/${tag}.tar.gz"
    fi
fi

if [ -z "$archive_url" ]; then
    echo "warning: GitHub releases API unavailable; using pinned DLSS SDK v${pinned_version}" >&2
    archive_url="$pinned_url"
fi

# --- download -----------------------------------------------------------------
mkdir -p "$dest"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

case "${archive_url%%\?*}" in
    *.zip) archive="$tmp/sdk.zip" ;;
    *)     archive="$tmp/sdk.tar.gz" ;;
esac

echo "Downloading NVIDIA DLSS SDK:"
echo "  $archive_url"
download "$archive_url" "$archive"

# --- extract include/ and lib/Linux_x86_64/ -----------------------------------
# Stage only the two directories we care about (the source archive is large),
# then copy them into place. The SDK root is located by finding the header, so
# this works whether or not the archive has a single top-level wrapper dir.
stage="$tmp/stage"
mkdir -p "$stage"
echo "Extracting include/ and lib/Linux_x86_64/ ..."
case "$archive" in
    *.zip)
        unzip -oq "$archive" -d "$stage" \
            '*/include/*' '*/lib/Linux_x86_64/*' 2>/dev/null ||
        unzip -oq "$archive" -d "$stage" \
            'include/*' 'lib/Linux_x86_64/*'
        ;;
    *)
        # The source archive wraps everything in a single top-level dir; try that
        # first and fall back to an unwrapped layout. (GNU tar treats a pattern
        # that matches nothing as an error, hence the two attempts.)
        tar -xzf "$archive" -C "$stage" --wildcards \
            '*/include/*' '*/lib/Linux_x86_64/*' 2>/dev/null ||
        tar -xzf "$archive" -C "$stage" --wildcards \
            'include/*' 'lib/Linux_x86_64/*'
        ;;
esac

header="$(find "$stage" -type f -name nvsdk_ngx.h -path '*/include/nvsdk_ngx.h' -print -quit)"
if [ -z "$header" ]; then
    echo "error: could not find include/nvsdk_ngx.h in the downloaded archive" >&2
    exit 1
fi
sdk_root="$(cd -- "$(dirname -- "$header")/.." && pwd)"

mkdir -p "$dest/include" "$dest/lib"
cp -a "$sdk_root/include/." "$dest/include/"
if [ -d "$sdk_root/lib/Linux_x86_64" ]; then
    cp -a "$sdk_root/lib/Linux_x86_64/." "$dest/lib/Linux_x86_64/"
fi

if [ ! -f "$dest/include/nvsdk_ngx.h" ]; then
    echo "error: extraction did not produce $dest/include/nvsdk_ngx.h" >&2
    exit 1
fi

echo "Done. NVIDIA DLSS SDK installed at: $dest"
echo "Configure with:"
echo "  cmake --preset <preset> -DDUSK_ENABLE_DLSS=ON -DDUSK_DLSS_SDK_DIR=$dest"
