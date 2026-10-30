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
