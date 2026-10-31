#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerates the small MSE byte stream fixtures of tests/media
# (test_ffmpeg_mse_stream.cpp). Needs an ffmpeg with libx264, libvpx and
# libopus: tests/media/data/generate.sh [path/to/ffmpeg]
set -euo pipefail

FFMPEG="${1:-ffmpeg}"
cd "$(dirname "${BASH_SOURCE[0]}")"
ff() { "${FFMPEG}" -hide_banner -loglevel error -y -bitexact "$@"; }

video=(-f lavfi -i testsrc2=size=192x108:rate=30)
audio=(-f lavfi -i sine=frequency=440:sample_rate=48000)

# H.264 + AAC, one moof+mdat per track and keyframe (YouTube-style DASH).
ff "${video[@]}" "${audio[@]}" -t 4 -c:v libx264 -preset ultrafast -g 30 -b:v 100k \
    -c:a aac -b:a 64k -movflags +frag_keyframe+empty_moov+default_base_moof+separate_moof \
    -fflags +bitexact -map_metadata -1 mse-avc-aac.mp4
# H.264 with styp + sidx boxes before every fragment.
ff "${video[@]}" -t 4 -c:v libx264 -preset ultrafast -g 30 -b:v 100k \
    -movflags +frag_keyframe+empty_moov+default_base_moof+dash+global_sidx \
    -fflags +bitexact -map_metadata -1 mse-avc-dash.mp4
# VP9 + Opus WebM written to a pipe: unknown-size Segment and Clusters.
ff "${video[@]}" "${audio[@]}" -t 4 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 100k -g 30 \
    -c:a libopus -b:a 32k -f webm -live 1 -cluster_size_limit 16000 -cluster_time_limit 1000 \
    -map_metadata -1 pipe:1 > mse-vp9-opus-live.webm
ls -l mse-*
