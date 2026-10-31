#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Syntax-checks the ffmpeg media engine against the real WebCore build
# configuration without touching build-wk-ps5/ or third_party/webkit:
# the compile flags of an existing WebCore object are read from
# build-wk-ps5/build.ninja (read-only), the headers the patch modifies come
# from an overlay built by sync-to-tree.sh, and each .cpp is compiled with
# -fsyntax-only. Needs WebCore's generated headers (PrivateHeaders,
# DerivedSources) to exist, i.e. the WebCore build must have started.
#
#   webkit-port/media/compile-check.sh [-c]   (-c: emit objects to build-media-check/obj)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD="${WEBKIT_BUILD_DIR:-${ROOT}/build-wk-ps5}"
SDK="${PS5_PAYLOAD_SDK:-/opt/ps5-payload-sdk}"
OUT="${ROOT}/build-media-check"
REFERENCE="Source/WebCore/CMakeFiles/WebCore.dir/platform/network/curl/CurlContext.cpp.o"
MODE=(-fsyntax-only)
[[ "${1:-}" == "-c" ]] && MODE=(-c)

[[ -f "${BUILD}/build.ninja" ]] || { echo "no ${BUILD}/build.ninja" >&2; exit 1; }

if grep -q 'MediaPlayerPrivateFFmpeg' "${ROOT}/third_party/webkit/Source/WebCore/platform/graphics/MediaPlayer.cpp"; then
    echo "the tree already contains the ffmpeg engine (patch 0900): run webkit-port/media/sync-to-tree.sh" >&2
    echo "and build it for real with scripts/ps5/build-webkit.sh webcore" >&2
    exit 1
fi
"${ROOT}/webkit-port/media/sync-to-tree.sh" --patch >/dev/null
TREE="${OUT}/patchwork/b/Source/WebCore/platform/graphics"

# Overlay for the two headers the patch changes (new enum values). Both
# spellings (<WebCore/X.h> and "X.h") resolve to the same inode, so
# #pragma once still holds.
OVERLAY="${OUT}/overlay"
mkdir -p "${OVERLAY}/WebCore"
for h in MediaPlayer.h MediaPlayerEnums.h; do
    cp -f -- "${TREE}/${h}" "${OVERLAY}/WebCore/${h}"
    ln -sf -- "WebCore/${h}" "${OVERLAY}/${h}"
done

rule="$(awk -v target="build ${REFERENCE}:" 'index($0, target) == 1 { p = 1 } p { print; if ($0 == "") exit }' "${BUILD}/build.ninja")"
[[ -n "${rule}" ]] || { echo "reference object ${REFERENCE} not in build.ninja" >&2; exit 1; }
# Drop WebCore's precompiled header (built for the unpatched tree) and the
# shell quoting ninja would have removed.
field() {
    printf '%s\n' "${rule}" | sed -n "s/^  $1 = //p" \
        | sed -E 's/-Xclang -include(-pch)? -Xclang [^ ]+//g; s/=""/=/g'
}
compiler="$(sed -n '/^rule CXX_COMPILER__WebCore_unscanned_Release/,/^$/s/^  command = \${LAUNCHER}\${CODE_CHECK}\([^ ]*\) \(--sysroot=[^ ]*\).*/\1 \2/p' "${BUILD}/CMakeFiles/rules.ninja")"
[[ -n "${compiler}" ]] || { echo "cannot find the WebCore compile rule" >&2; exit 1; }

sources=("${TREE}"/ffmpeg/*.cpp)
# The flags contain glob characters ([[clang::trivial_abi]]).
set -f
# shellcheck disable=SC2206
cmd=(${compiler} -I"${OVERLAY}" $(field DEFINES) $(field INCLUDES) $(field FLAGS)
    -DUSE_FFMPEG=1 -DFFMPEG_MEDIA_SDL_AUDIO=1
    -I"${TREE}/ffmpeg" -isystem "${SDK}/target/user/homebrew/include")

mkdir -p "${OUT}/obj"
status=0
for src in "${sources[@]}"; do
    name="$(basename "${src}" .cpp)"
    printf '== %s\n' "${name}.cpp"
    extra=()
    [[ "${MODE[0]}" == "-c" ]] && extra=(-o "${OUT}/obj/${name}.o")
    if ! (cd "${BUILD}" && "${cmd[@]}" "${MODE[@]}" "${extra[@]}" "${src}"); then
        status=1
    fi
done
exit "${status}"
