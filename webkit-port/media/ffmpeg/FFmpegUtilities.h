// SPDX-License-Identifier: GPL-3.0-or-later
//
// ffmpeg RAII helpers and timestamp conversion shared by the ffmpeg media
// engine (demuxer, decoders, player).

#pragma once

#if ENABLE(VIDEO) && USE(FFMPEG)

#include <cmath>
#include <limits>
#include <memory>
#include <wtf/text/WTFString.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace WebCore {

struct AVPacketDeleter {
    void operator()(AVPacket* packet) const { av_packet_free(&packet); }
};
using AVPacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;

struct AVFrameDeleter {
    void operator()(AVFrame* frame) const { av_frame_free(&frame); }
};
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

struct AVCodecContextDeleter {
    void operator()(AVCodecContext* context) const { avcodec_free_context(&context); }
};
using AVCodecContextPtr = std::unique_ptr<AVCodecContext, AVCodecContextDeleter>;

struct AVCodecParametersDeleter {
    void operator()(AVCodecParameters* parameters) const { avcodec_parameters_free(&parameters); }
};
using AVCodecParametersPtr = std::unique_ptr<AVCodecParameters, AVCodecParametersDeleter>;

// Immutable description of one demuxed stream, handed from the demuxer to a
// decoder. Times are in seconds on the media timeline: stream timestamps
// minus the container start time, so the media starts at 0.
struct FFmpegStreamInfo {
    int index { -1 };
    AVRational timeBase { 0, 1 };
    double startTime { 0 };
    AVCodecParametersPtr parameters;

    double toSeconds(int64_t timestamp) const
    {
        if (timestamp == AV_NOPTS_VALUE || !timeBase.den)
            return std::numeric_limits<double>::quiet_NaN();
        return timestamp * av_q2d(timeBase) - startTime;
    }
};

// libavcodec's native AV1 decoder only drives hardware accelerators, so AV1
// needs libdav1d or libaom (absent from the PS5 SDK build).
inline const AVCodec* findSoftwareDecoder(AVCodecID codecId)
{
    if (codecId == AV_CODEC_ID_AV1) {
        if (auto* codec = avcodec_find_decoder_by_name("libdav1d"))
            return codec;
        return avcodec_find_decoder_by_name("libaom-av1");
    }
    return avcodec_find_decoder(codecId);
}

inline String ffmpegErrorString(int error)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = { };
    av_strerror(error, buffer, sizeof(buffer));
    return String::fromLatin1(buffer);
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
