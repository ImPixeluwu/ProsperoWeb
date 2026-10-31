#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The ffmpeg media engine is edited in webkit-port/media/ffmpeg (source of
# truth; the MSE engine in ffmpeg/mse) and shipped to WebKit as patch 0900,
# which adds the files under Source/WebCore/platform/graphics/ffmpeg/ and
# wires them up. The USE_FFMPEG and ENABLE_MEDIA_SOURCE options live in
# patch 0001 (OptionsPlayStation.cmake).
#
#   webkit-port/media/sync-to-tree.sh [--sync]  copy ffmpeg/ and FFmpeg.cmake into the
#                                               patched tree, then regenerate all patches
#                                               with scripts/ps5/export-webkit-patches.sh
#   webkit-port/media/sync-to-tree.sh --patch   bootstrap: generate patch 0900 from a tree
#                                               that does not contain the engine yet
#   webkit-port/media/sync-to-tree.sh --apply   apply patch 0900 to such a tree
#
# --patch never touches third_party/webkit: it copies the files the patch
# modifies into build-media-check/patchwork/{a,b}, edits b/ and diffs. b/
# doubles as the header overlay for the compile check (see README.md).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WEBKIT_DIR="${WEBKIT_DIR:-${ROOT}/third_party/webkit}"
SRC_DIR="${ROOT}/webkit-port/media/ffmpeg"
CMAKE_TEMPLATE="${ROOT}/webkit-port/media/FFmpeg.cmake"
PATCH="${ROOT}/webkit-port/patches/0900-ffmpeg-media-player.patch"
WORK="${ROOT}/build-media-check/patchwork"

MODIFIED=(
    Source/WebCore/PlatformPlayStation.cmake
    Source/WebCore/platform/graphics/MediaPlayer.cpp
    Source/WebCore/platform/graphics/MediaPlayer.h
    Source/WebCore/platform/graphics/MediaPlayerEnums.h
    Source/WebKit/Shared/WebCoreArgumentCoders.serialization.in
    Source/WebKit/WebProcess/GPU/media/MediaPlayerPrivateRemote.cpp
)

die() { printf 'sync-to-tree: error: %s\n' "$*" >&2; exit 1; }

copy_sources() {
    mkdir -p "$1/mse"
    cp -- "${SRC_DIR}"/*.h "${SRC_DIR}"/*.cpp "$1/"
    cp -- "${SRC_DIR}"/mse/*.h "${SRC_DIR}"/mse/*.cpp "$1/mse/"
}

# Wires the engines into WebCore/WebKit under the tree root $1. Each edit is
# skipped when already present, so this both bootstraps a pristine tree and
# upgrades a tree that has the progressive engine only (adding the MSE one).
wire_engine() {
    python3 -I - "$1" <<'PY'
import sys, pathlib
base = pathlib.Path(sys.argv[1])

def edit(path, anchor, replacement, done=None):
    p = base / path
    text = p.read_text()
    if (done or replacement) in text:
        return
    if text.count(anchor) != 1:
        sys.exit(f"sync-to-tree: anchor not found exactly once in {path}: {anchor!r}")
    p.write_text(text.replace(anchor, replacement))

edit("Source/WebCore/PlatformPlayStation.cmake",
     "if (USE_COORDINATED_GRAPHICS)\n    include(platform/CoordinatedGraphics.cmake)\nendif ()\n",
     "if (USE_COORDINATED_GRAPHICS)\n    include(platform/CoordinatedGraphics.cmake)\nendif ()\n\n"
     "if (USE_FFMPEG)\n    include(platform/FFmpeg.cmake)\nendif ()\n")

mp = "Source/WebCore/platform/graphics/MediaPlayer.cpp"
edit(mp, '#if USE(EXTERNAL_HOLEPUNCH)\n#include "MediaPlayerPrivateHolePunch.h"\n#endif\n',
     '#if USE(EXTERNAL_HOLEPUNCH)\n#include "MediaPlayerPrivateHolePunch.h"\n#endif\n\n'
     '#if USE(FFMPEG)\n#include "MediaPlayerPrivateFFmpeg.h"\n#endif\n')
edit(mp, '#if USE(FFMPEG)\n#include "MediaPlayerPrivateFFmpeg.h"\n#endif\n',
     '#if USE(FFMPEG)\n#include "MediaPlayerPrivateFFmpeg.h"\n#if ENABLE(MEDIA_SOURCE)\n#include "MediaPlayerPrivateFFmpegMSE.h"\n#endif\n#endif\n')
edit(mp, '#if USE(EXTERNAL_HOLEPUNCH)\n    MediaPlayerPrivateHolePunch::registerMediaEngine(addMediaEngine);\n#endif\n',
     '#if USE(EXTERNAL_HOLEPUNCH)\n    MediaPlayerPrivateHolePunch::registerMediaEngine(addMediaEngine);\n#endif\n\n'
     '#if USE(FFMPEG)\n    MediaPlayerPrivateFFmpeg::registerMediaEngine(addMediaEngine);\n#endif\n')
edit(mp, '#if USE(FFMPEG)\n    MediaPlayerPrivateFFmpeg::registerMediaEngine(addMediaEngine);\n#endif\n',
     '#if USE(FFMPEG)\n    MediaPlayerPrivateFFmpeg::registerMediaEngine(addMediaEngine);\n#if ENABLE(MEDIA_SOURCE)\n'
     '    MediaPlayerPrivateFFmpegMSE::registerMediaEngine(addMediaEngine);\n#endif\n#endif\n')

mh = "Source/WebCore/platform/graphics/MediaPlayer.h"
edit(mh, "    GStreamer,\n    Remote\n};", "    GStreamer,\n    Remote,\n    FFmpeg\n};")
edit(mh, "    WirelessPlayback,\n    Remote\n};", "    WirelessPlayback,\n    Remote,\n    FFmpeg\n};", "    WirelessPlayback,\n    Remote,\n    FFmpeg")
edit(mh, "    WirelessPlayback,\n    Remote,\n    FFmpeg\n};", "    WirelessPlayback,\n    Remote,\n    FFmpeg,\n    FFmpegMSE\n};")

me = "Source/WebCore/platform/graphics/MediaPlayerEnums.h"
edit(me, "    CocoaWebM,\n    WirelessPlayback,\n};", "    CocoaWebM,\n    WirelessPlayback,\n    FFmpeg,\n};", "    WirelessPlayback,\n    FFmpeg,")
edit(me, "    WirelessPlayback,\n    FFmpeg,\n};", "    WirelessPlayback,\n    FFmpeg,\n    FFmpegMSE,\n};")

ser = "Source/WebKit/Shared/WebCoreArgumentCoders.serialization.in"
edit(ser, "    CocoaWebM,\n    WirelessPlayback\n};", "    CocoaWebM,\n    WirelessPlayback,\n    FFmpeg\n};", "    WirelessPlayback,\n    FFmpeg")
edit(ser, "    WirelessPlayback,\n    FFmpeg\n};", "    WirelessPlayback,\n    FFmpeg,\n    FFmpegMSE\n};")

remote = "Source/WebKit/WebProcess/GPU/media/MediaPlayerPrivateRemote.cpp"
edit(remote, "    case MediaPlayerMediaEngineIdentifier::WirelessPlayback:\n        return false;\n",
     "    case MediaPlayerMediaEngineIdentifier::WirelessPlayback:\n"
     "    case MediaPlayerMediaEngineIdentifier::FFmpeg:\n        return false;\n",
     "    case MediaPlayerMediaEngineIdentifier::FFmpeg:\n")
edit(remote, "    case MediaPlayerMediaEngineIdentifier::FFmpeg:\n        return false;\n",
     "    case MediaPlayerMediaEngineIdentifier::FFmpeg:\n    case MediaPlayerMediaEngineIdentifier::FFmpegMSE:\n        return false;\n")
PY
}

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

    wire_engine "${WORK}/b"

    copy_sources "${WORK}/b/Source/WebCore/platform/graphics/ffmpeg"
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
sink with an audio-master A/V clock. Media Source Extensions through
MediaPlayerPrivateFFmpegMSE (ffmpeg/mse/: fragmented MP4 and WebM source
buffers parsed by libavformat, fed to the same decoders). No EME or
hardware decoding.

USE_FFMPEG (patch 0001) defaults to PS5_HOMEBREW and requires
ENABLE_VIDEO. New MediaPlayerType values FFmpeg and FFmpegMSE,
MediaEngineIdentifier values FFmpeg and FFmpegMSE, MediaPlatformType
value FFmpeg.

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

sync_tree() {
    local dst="${WEBKIT_DIR}/Source/WebCore/platform/graphics/ffmpeg"
    grep -q 'MediaPlayerPrivateFFmpeg' "${WEBKIT_DIR}/Source/WebCore/platform/graphics/MediaPlayer.cpp" \
        || die "the tree does not contain the ffmpeg engine; run scripts/ps5/fetch-webkit.sh first"
    copy_sources "${dst}"
    cp -- "${CMAKE_TEMPLATE}" "${WEBKIT_DIR}/Source/WebCore/platform/FFmpeg.cmake"
    wire_engine "${WEBKIT_DIR}"
    "${ROOT}/scripts/ps5/export-webkit-patches.sh"
}

case "${1:---sync}" in
    --sync)  sync_tree ;;
    --patch) generate ;;
    --apply) apply_patch ;;
    *) die "usage: $0 [--sync|--patch|--apply]" ;;
esac
