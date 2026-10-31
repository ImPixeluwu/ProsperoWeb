#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Syntax-checks the ffmpeg media engine against the real WebCore build
# configuration without touching build-wk-ps5/ or third_party/webkit:
# the compile flags of an existing WebCore object are read from
# build-wk-ps5/build.ninja (read-only), the headers the patch modifies come
# from an overlay, and each .cpp is compiled with -fsyntax-only. Needs
# WebCore's generated headers (PrivateHeaders, DerivedSources) to exist,
# i.e. the WebCore build must have started.
#
# Two modes:
#  - bootstrap (the tree has no engine yet): the overlay is the patchwork
#    tree of sync-to-tree.sh --patch, the sources are its copies;
#  - integrated (patch 0900 applied): the sources are webkit-port/media/ffmpeg
#    and ffmpeg/mse themselves, the overlay is the tree's MediaPlayer.h and
#    MediaPlayerEnums.h with the MSE engine enum values added and a copy of
#    cmakeconfig.h with ENABLE_MEDIA_SOURCE forced to 1, so the MSE engine is
#    checked before the (full) rebuild that enabling MEDIA_SOURCE causes.
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

WEBKIT_GRAPHICS="${ROOT}/third_party/webkit/Source/WebCore/platform/graphics"
OVERLAY="${OUT}/overlay"
rm -f -- "${OVERLAY}"/*.h "${OVERLAY}"/WebCore/*.h
mkdir -p "${OVERLAY}/WebCore"
overlay_headers=(MediaPlayer.h MediaPlayerEnums.h)
if grep -q 'MediaPlayerPrivateFFmpeg' "${WEBKIT_GRAPHICS}/MediaPlayer.cpp"; then
    MODE_NAME=integrated
    SRC="${ROOT}/webkit-port/media/ffmpeg"
    for h in MediaPlayer.h MediaPlayerEnums.h; do
        cp -f -- "${WEBKIT_GRAPHICS}/${h}" "${OVERLAY}/WebCore/${h}"
    done
    python3 -I - "${OVERLAY}/WebCore" <<'PY'
import sys, pathlib
base = pathlib.Path(sys.argv[1])
def edit(name, anchor, replacement, done):
    p = base / name
    text = p.read_text()
    if done in text:
        return
    if text.count(anchor) != 1:
        sys.exit(f"compile-check: anchor not found in {name}: {anchor!r}")
    p.write_text(text.replace(anchor, replacement))
edit("MediaPlayer.h", "    GStreamer,\n    Remote\n};", "    GStreamer,\n    Remote,\n    FFmpeg\n};", "    Remote,\n    FFmpeg\n};\n\nenum class MediaPlayerType")
edit("MediaPlayer.h", "    Remote,\n    FFmpeg\n};\n\nusing TrackID", "    Remote,\n    FFmpeg,\n    FFmpegMSE\n};\n\nusing TrackID", "FFmpegMSE\n};\n\nusing TrackID")
edit("MediaPlayerEnums.h", "    FFmpeg,\n};", "    FFmpeg,\n    FFmpegMSE,\n};", "FFmpegMSE,")
PY
    sed -E 's/^#define ENABLE_MEDIA_SOURCE 0$/#define ENABLE_MEDIA_SOURCE 1/' "${BUILD}/cmakeconfig.h" > "${OVERLAY}/cmakeconfig.h"
    # Until patch 0901 is in the tree: a dllexport member of a dllexport
    # class is an error for the PlayStation target.
    sed -E 's/^    WEBCORE_EXPORT static PlatformTimeRanges computeBufferedRanges/    static PlatformTimeRanges computeBufferedRanges/' \
        "${WEBKIT_GRAPHICS}/MediaSourcePrivate.h" > "${OVERLAY}/WebCore/MediaSourcePrivate.h"
    overlay_headers+=(MediaSourcePrivate.h)
    sources=("${SRC}"/*.cpp "${SRC}"/mse/*.cpp)
    includes=(-I"${SRC}" -I"${SRC}/mse")
else
    MODE_NAME=bootstrap
    "${ROOT}/webkit-port/media/sync-to-tree.sh" --patch >/dev/null
    TREE="${OUT}/patchwork/b/Source/WebCore/platform/graphics"
    # Overlay for the two headers the patch changes (new enum values).
    for h in MediaPlayer.h MediaPlayerEnums.h; do
        cp -f -- "${TREE}/${h}" "${OVERLAY}/WebCore/${h}"
    done
    rm -f -- "${OVERLAY}/cmakeconfig.h"
    sources=("${TREE}"/ffmpeg/*.cpp "${TREE}"/ffmpeg/mse/*.cpp)
    includes=(-I"${TREE}/ffmpeg" -I"${TREE}/ffmpeg/mse")
fi
# Both spellings (<WebCore/X.h> and "X.h") resolve to the same inode, so
# #pragma once still holds.
for h in "${overlay_headers[@]}"; do
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

# The flags contain glob characters ([[clang::trivial_abi]]).
set -f
# shellcheck disable=SC2206
# The engine's own directories come first: in integrated mode WebCore's
# include path also has the tree's (older) copies of the same headers.
cmd=(${compiler} -I"${OVERLAY}" "${includes[@]}" $(field DEFINES) $(field INCLUDES) $(field FLAGS)
    -DUSE_FFMPEG=1 -DFFMPEG_MEDIA_SDL_AUDIO=1
    -isystem "${SDK}/target/user/homebrew/include")
echo "compile-check: ${MODE_NAME} mode, ${#sources[@]} sources"

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
