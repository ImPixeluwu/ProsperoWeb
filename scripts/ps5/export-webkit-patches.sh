#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Developer helper: regenerate webkit-port/patches/*.patch from the local
# changes in third_party/webkit. Each patch owns a fixed list of paths
# (TOPICS below); the explanatory header of an existing patch (everything
# before the first "diff --git") is preserved. Updates the fetch stamp, since
# the tree then matches the patch set by construction.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WEBKIT_DIR="${WEBKIT_DIR:-${ROOT}/third_party/webkit}"
PATCH_DIR="${ROOT}/webkit-port/patches"

# patch-name|path path ...
TOPICS=(
    "0001-cmake-playstation-homebrew-options|Source/cmake/OptionsPlayStation.cmake"
    "0002-wtf-posix-osallocator|Source/WTF/wtf/PlatformPlayStation.cmake"
    "0003-jsc-generic-shell|Source/JavaScriptCore/PlatformPlayStation.cmake Source/JavaScriptCore/shell/PlatformPlayStation.cmake"
    "0004-webcore-curl-ca-bundle|Source/WebCore/platform/network/playstation/CurlSSLHandlePlayStation.cpp"
    "0005-webkit-static-multicall-processes|Source/WebKit/CMakeLists.txt Source/WebKit/PlatformPlayStation.cmake Source/WebKit/UIProcess/Launcher/playstation/ProcessLauncherPlayStation.cpp Source/WebKit/WebProcess/EntryPoint/playstation/WebProcessMain.cpp Source/WebKit/NetworkProcess/EntryPoint/playstation/NetworkProcessMain.cpp"
    "0006-wtf-payload-sdk-compat|Source/WTF/wtf/Compiler.h Source/WTF/wtf/AvailableMemory.cpp Source/WTF/wtf/RAMSize.cpp Source/WTF/wtf/unix/MemoryPressureHandlerUnix.cpp Source/WTF/wtf/playstation/UniStdExtrasPlayStation.cpp Source/WTF/wtf/playstation/LanguagePlayStation.cpp"
    "0007-webcore-playstation-build-fixes|Source/WebCore/platform/graphics/skia/PlatformDisplaySkia.cpp Source/WebCore/platform/network/DNS.cpp Source/WebCore/page/Page.cpp"
    "0008-skia-freebsd-malloc-usable-size|Source/ThirdParty/skia/src/ports/SkMemory_malloc.cpp"
    "0009-webkit-static-injected-bundle|Source/WebKit/WebProcess/InjectedBundle/playstation/InjectedBundlePlayStation.cpp"
    "0010-webcore-timing-function-destructors|Source/WebCore/platform/animation/TimingFunction.h Source/WebCore/platform/animation/TimingFunction.cpp"
    "0011-webkit-playstation-page-api|Source/WebKit/UIProcess/API/C/playstation/WKPagePrivatePlayStation.h Source/WebKit/UIProcess/API/C/playstation/WKPagePrivatePlayStation.cpp"
    # Generated from webkit-port/media/ffmpeg: edit there, then run
    # webkit-port/media/sync-to-tree.sh (which calls this script).
    "0900-ffmpeg-media-player|Source/WebCore/PlatformPlayStation.cmake Source/WebCore/platform/FFmpeg.cmake Source/WebCore/platform/graphics/MediaPlayer.cpp Source/WebCore/platform/graphics/MediaPlayer.h Source/WebCore/platform/graphics/MediaPlayerEnums.h Source/WebKit/Shared/WebCoreArgumentCoders.serialization.in Source/WebKit/WebProcess/GPU/media/MediaPlayerPrivateRemote.cpp $(cd "${ROOT}/webkit-port/media/ffmpeg" && printf 'Source/WebCore/platform/graphics/ffmpeg/%s ' *.h *.cpp)"
)

mkdir -p "${PATCH_DIR}"
covered=()
for t in "${TOPICS[@]}"; do
    name="${t%%|*}"
    read -r -a paths <<< "${t#*|}"
    covered+=("${paths[@]}")
    out="${PATCH_DIR}/${name}.patch"
    header=""
    if [[ -f "${out}" ]]; then
        header="$(sed '/^diff --git /,$d' "${out}")"
    fi
    # Make new (untracked) files visible to git diff.
    for p in "${paths[@]}"; do
        if [[ -e "${WEBKIT_DIR}/${p}" ]] && ! git -C "${WEBKIT_DIR}" ls-files --error-unmatch -- "${p}" >/dev/null 2>&1; then
            git -C "${WEBKIT_DIR}" add -N -- "${p}"
        fi
    done
    diff="$(git -C "${WEBKIT_DIR}" diff --no-color --no-ext-diff -- "${paths[@]}")"
    if [[ -z "${diff}" ]]; then
        echo "warning: ${name}: no changes" >&2
        continue
    fi
    {
        if [[ -n "${header}" ]]; then printf '%s\n' "${header}"; fi
        printf '%s\n' "${diff}"
    } > "${out}"
    echo "wrote ${out#${ROOT}/}"
done

# Refuse to silently drop changes that no topic owns.
stray="$(git -C "${WEBKIT_DIR}" status --porcelain -- Source Tools CMakeLists.txt | awk '{print $2}' | grep -vxF -f <(printf '%s\n' "${covered[@]}") || true)"
if [[ -n "${stray}" ]]; then
    echo "error: changes not covered by any patch topic:" >&2
    echo "${stray}" >&2
    exit 1
fi

(cd "${PATCH_DIR}" && cat -- *.patch) | sha256sum | cut -d' ' -f1 > "${WEBKIT_DIR}/.ps5-patches.stamp"
