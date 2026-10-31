// SPDX-License-Identifier: GPL-3.0-or-later
//
// Parser of one MSE SourceBuffer's byte stream (fragmented MP4 or WebM).
// SegmentScanner cuts the appended bytes into complete initialization and
// media segments; each initialization segment is opened with libavformat
// (mov or matroska demuxer) to describe the tracks, and each batch of media
// segments is demuxed together with the current initialization segment from
// an in-memory AVIOContext. libavformat thus always sees a complete, seekable
// file and never waits for data; demuxing is synchronous on the caller's
// thread.
//
// Uses only ffmpeg and the C++ library (no WebCore/WTF), so it also runs in
// a host harness linked against a desktop libavformat.

#pragma once

#if ENABLE(VIDEO) && USE(FFMPEG) && ENABLE(MEDIA_SOURCE)

#include "FFmpegMSEParsing.h"
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace WebCore {

struct FFmpegParsedTrack {
    FFmpegParsedTrack() = default;
    FFmpegParsedTrack(const FFmpegParsedTrack&);
    FFmpegParsedTrack& operator=(const FFmpegParsedTrack&) = delete;
    ~FFmpegParsedTrack();

    // MP4 track_ID / Matroska TrackNumber (index + 1 when absent).
    uint64_t trackID { 0 };
    AVMediaType type { AVMEDIA_TYPE_UNKNOWN };
    // Owned copy (codec id, extradata, size, sample rate, channels, ...).
    AVCodecParameters* parameters { nullptr };
    AVRational timeBase { 0, 1 };
    AVRational sampleAspectRatio { 0, 1 };
    std::string codecName; // libavcodec descriptor name, "?" when unknown
    std::string fourCC; // MediaDescription codec ("avc1", "mp4a", "vp09", ...)
};

struct FFmpegParsedInitializationSegment {
    std::vector<FFmpegParsedTrack> tracks;
    double durationSeconds { 0 }; // NaN when the segment has none
};

class FFmpegAppendParserClient {
public:
    virtual ~FFmpegAppendParserClient() = default;
    virtual void appendParserDidParseInitializationSegment(const FFmpegParsedInitializationSegment&) = 0;
    // `packet` timestamps and duration are in `track.timeBase`; missing
    // durations are already filled in. The client may av_packet_move_ref()
    // the packet; the parser unrefs whatever is left.
    virtual void appendParserDidParseSample(const FFmpegParsedTrack&, AVPacket* packet) = 0;
};

class FFmpegAppendParser {
public:
    explicit FFmpegAppendParser(FFmpegMedia::ByteStreamFormat);
    ~FFmpegAppendParser();

    // Parses `size` more bytes; returns false on a parse error (error()).
    // Callbacks run synchronously, in stream order.
    bool append(const uint8_t* data, size_t size, FFmpegAppendParserClient&);
    // resetParserState(): drop partial data, keep the initialization segment.
    void reset();

    bool hasInitializationSegment() const { return !m_initialization.empty(); }
    const FFmpegParsedInitializationSegment& initializationSegment() const { return m_current; }
    const std::string& error() const { return m_error; }
    size_t pendingBytes() const { return m_scanner.pendingBytes(); }

    unsigned lastAppendSamples() const { return m_lastAppendSamples; }

private:
    bool parseInitialization(const std::vector<uint8_t>&, FFmpegAppendParserClient&);
    bool demuxMedia(const std::vector<uint8_t>&, FFmpegAppendParserClient&);
    const AVInputFormat* inputFormat() const;

    const FFmpegMedia::ByteStreamFormat m_format;
    FFmpegMedia::SegmentScanner m_scanner;
    std::vector<uint8_t> m_initialization;
    FFmpegParsedInitializationSegment m_current;
    std::string m_error;
    unsigned m_lastAppendSamples { 0 };
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG) && ENABLE(MEDIA_SOURCE)
