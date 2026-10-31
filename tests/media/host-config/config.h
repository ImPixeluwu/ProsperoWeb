// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stand-in for WebCore's config.h, so FFmpegAppendParser.cpp builds in the
// host tests against the desktop libavformat.
#pragma once

#define ENABLE(feature) (defined ENABLE_##feature && ENABLE_##feature)
#define USE(feature) (defined USE_##feature && USE_##feature)

#define ENABLE_VIDEO 1
#define ENABLE_MEDIA_SOURCE 1
#define USE_FFMPEG 1
