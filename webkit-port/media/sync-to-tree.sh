#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The ffmpeg media engine is edited in webkit-port/media/ffmpeg (source of
# truth) and shipped to WebKit as one patch that adds the files under
# Source/WebCore/platform/graphics/ffmpeg/ and wires them up.
#
#   webkit-port/media/sync-to-tree.sh [--patch]   regenerate the patch (default)
#   webkit-port/media/sync-to-tree.sh --apply     apply the patch to third_party/webkit
#
# --patch never touches third_party/webkit: it copies the files the patch
# modifies into build-media-check/patchwork/{a,b}, edits b/ and diffs. b/
# doubles as the header overlay for the compile check (see README.md).
# The tree must not contain the patch yet (run it before --apply).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WEBKIT_DIR="${WEBKIT_DIR:-${ROOT}/third_party/webkit}"
SRC_DIR="${ROOT}/webkit-port/media/ffmpeg"
CMAKE_TEMPLATE="${ROOT}/webkit-port/media/FFmpeg.cmake"
PATCH="${ROOT}/webkit-port/patches/pending/0900-ffmpeg-media-player.patch"
WORK="${ROOT}/build-media-check/patchwork"

MODIFIED=(
    Source/cmake/OptionsPlayStation.cmake
    Source/WebCore/PlatformPlayStation.cmake
    Source/WebCore/platform/graphics/MediaPlayer.cpp
    Source/WebCore/platform/graphics/MediaPlayer.h
    Source/WebCore/platform/graphics/MediaPlayerEnums.h
    Source/WebKit/Shared/WebCoreArgumentCoders.serialization.in
    Source/WebKit/WebProcess/GPU/media/MediaPlayerPrivateRemote.cpp
)

die() { printf 'sync-to-tree: error: %s\n' "$*" >&2; exit 1; }

generate() {
    [[ -d "${WEBKIT_DIR}/Source" ]] || die "${WEBKIT_DIR} is not a WebKit checkout"
    if grep -q 'MediaPlayerPrivateFFmpeg' "${WEBKIT_DIR}/Source/WebCore/platform/graphics/MediaPlayer.cpp"; then
        die "the tree already contains the ffmpeg engine; regenerate from a tree without it"
    fi
    # Only ever removes our own scratch directory.
    rm -rf -- "${WORK}"
    mkdir -p "${WORK}/a" "${WORK}/b"
    for f in "${MODIFIED[@]}"; do
        mkdir -p "$(dirname "${WORK}/a/${f}")" "$(dirname "${WORK}/b/${f}")"
        cp -- "${WEBKIT_DIR}/${f}" "${WORK}/a/${f}"
        cp -- "${WEBKIT_DIR}/${f}" "${WORK}/b/${f}"
    done

    python3 -I - "${WORK}/b" <<'PY'
import sys, pathlib
base = pathlib.Path(sys.argv[1])

def edit(path, anchor, replacement, count=1):
    p = base / path
    text = p.read_text()
    if text.count(anchor) != count:
        sys.exit(f"sync-to-tree: anchor not found exactly {count}x in {path}: {anchor!r}")
    p.write_text(text.replace(anchor, replacement))

edit("Source/cmake/OptionsPlayStation.cmake",
     'WEBKIT_OPTION_DEFINE(ENABLE_STATIC_JSC "Control whether to build a non-shared JSC" PUBLIC ON)\n',
     'WEBKIT_OPTION_DEFINE(ENABLE_STATIC_JSC "Control whether to build a non-shared JSC" PUBLIC ON)\n'
     '# ffmpeg media engine (platform/graphics/ffmpeg): software playback of\n'
     '# progressive media and HLS, the only media backend of the homebrew build.\n'
     'WEBKIT_OPTION_DEFINE(USE_FFMPEG "Whether to use the ffmpeg media engine" PRIVATE ${PS5_HOMEBREW})\n'
     'WEBKIT_OPTION_DEPEND(USE_FFMPEG ENABLE_VIDEO)\n')

edit("Source/WebCore/PlatformPlayStation.cmake",
     "if (USE_COORDINATED_GRAPHICS)\n    include(platform/CoordinatedGraphics.cmake)\nendif ()\n",
     "if (USE_COORDINATED_GRAPHICS)\n    include(platform/CoordinatedGraphics.cmake)\nendif ()\n\n"
     "if (USE_FFMPEG)\n    include(platform/FFmpeg.cmake)\nendif ()\n")

edit("Source/WebCore/platform/graphics/MediaPlayer.cpp",
     '#if USE(EXTERNAL_HOLEPUNCH)\n#include "MediaPlayerPrivateHolePunch.h"\n#endif\n',
     '#if USE(EXTERNAL_HOLEPUNCH)\n#include "MediaPlayerPrivateHolePunch.h"\n#endif\n\n'
     '#if USE(FFMPEG)\n#include "MediaPlayerPrivateFFmpeg.h"\n#endif\n')
edit("Source/WebCore/platform/graphics/MediaPlayer.cpp",
     '#if USE(EXTERNAL_HOLEPUNCH)\n    MediaPlayerPrivateHolePunch::registerMediaEngine(addMediaEngine);\n#endif\n',
     '#if USE(EXTERNAL_HOLEPUNCH)\n    MediaPlayerPrivateHolePunch::registerMediaEngine(addMediaEngine);\n#endif\n\n'
     '#if USE(FFMPEG)\n    MediaPlayerPrivateFFmpeg::registerMediaEngine(addMediaEngine);\n#endif\n')

edit("Source/WebCore/platform/graphics/MediaPlayer.h",
     "    WirelessPlayback,\n    Remote\n};",
     "    WirelessPlayback,\n    Remote,\n    FFmpeg\n};")

edit("Source/WebCore/platform/graphics/MediaPlayerEnums.h",
     "    CocoaWebM,\n    WirelessPlayback,\n};",
     "    CocoaWebM,\n    WirelessPlayback,\n    FFmpeg,\n};")

edit("Source/WebKit/Shared/WebCoreArgumentCoders.serialization.in",
     "    CocoaWebM,\n    WirelessPlayback\n};",
     "    CocoaWebM,\n    WirelessPlayback,\n    FFmpeg\n};")

edit("Source/WebKit/WebProcess/GPU/media/MediaPlayerPrivateRemote.cpp",
     "    case MediaPlayerMediaEngineIdentifier::WirelessPlayback:\n        return false;\n",
     "    case MediaPlayerMediaEngineIdentifier::WirelessPlayback:\n"
     "    case MediaPlayerMediaEngineIdentifier::FFmpeg:\n        return false;\n")
PY

    mkdir -p "${WORK}/b/Source/WebCore/platform/graphics/ffmpeg"
    cp -- "${SRC_DIR}"/*.h "${SRC_DIR}"/*.cpp "${WORK}/b/Source/WebCore/platform/graphics/ffmpeg/"
    cp -- "${CMAKE_TEMPLATE}" "${WORK}/b/Source/WebCore/platform/FFmpeg.cmake"

    mkdir -p "$(dirname "${PATCH}")"
    {
        cat <<'HDR'
Subject: WebCore: ffmpeg media engine (USE_FFMPEG) for PS5_HOMEBREW

Adds MediaPlayerPrivateFFmpeg, a software media player built on the
payload SDK's static ffmpeg 7.0: progressive MP4/WebM/MP3/Ogg/FLAC/WAV
and HLS through libavformat (media requests go through WebCore's
PlatformMediaResourceLoader), libavcodec decoding, libswscale to BGRA
painted via NativeImage, libswresample to an SDL2 (or silent) audio
sink with an audio-master A/V clock. No MSE, EME or hardware decoding.

USE_FFMPEG defaults to PS5_HOMEBREW and requires ENABLE_VIDEO. New
MediaPlayerType/MediaEngineIdentifier value FFmpeg.

Generated by webkit-port/media/sync-to-tree.sh from webkit-port/media/ffmpeg;
edit the files there, not this patch.
HDR
        (cd "${WORK}" && git diff --no-index --no-color --no-ext-diff a b || true) \
            | sed -E 's#^diff --git a/[ab]/(.*) b/[ab]/(.*)$#diff --git a/\1 b/\2#; s#^--- a/[ab]/#--- a/#; s#^\+\+\+ b/[ab]/#+++ b/#'
    } > "${PATCH}"
    git -C "${WEBKIT_DIR}" apply --check "${PATCH}" || die "generated patch does not apply to ${WEBKIT_DIR}"
    echo "wrote ${PATCH#${ROOT}/} ($(grep -c '^diff --git' "${PATCH}") files)"
}

apply_patch() {
    [[ -f "${PATCH}" ]] || die "no patch; run without arguments first"
    git -C "${WEBKIT_DIR}" apply --check "${PATCH}"
    git -C "${WEBKIT_DIR}" apply "${PATCH}"
    echo "applied ${PATCH#${ROOT}/} to ${WEBKIT_DIR#${ROOT}/}"
}

case "${1:---patch}" in
    --patch) generate ;;
    --apply) apply_patch ;;
    *) die "usage: $0 [--patch|--apply]" ;;
esac
