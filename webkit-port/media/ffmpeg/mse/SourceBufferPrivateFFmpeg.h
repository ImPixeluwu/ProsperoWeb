// SPDX-License-Identifier: GPL-3.0-or-later
//
// SourceBufferPrivate of the ffmpeg media engine. appendBuffer() bytes go to
// an FFmpegAppendParser (fragmented MP4 or WebM through libavformat); the
// initialization segment becomes track descriptions, every demuxed packet a
// MediaSampleFFmpeg handed to the generic SourceBufferPrivate code, which
// owns the sample maps, timestampOffset, append windows, coded frame removal
// and eviction. Samples the generic code enqueues go to
// MediaPlayerPrivateFFmpegMSE's decoder queues. Main thread only.

#pragma once

#if ENABLE(VIDEO) && USE(FFMPEG) && ENABLE(MEDIA_SOURCE)

#include "FFmpegAppendParser.h"
#include "SourceBufferPrivate.h"
#include <atomic>
#include <wtf/HashMap.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

class ContentType;
class FFmpegMSETrackFormat;
class MediaPlayerPrivateFFmpegMSE;
class MediaSourcePrivateFFmpeg;

class SourceBufferPrivateFFmpeg final : public SourceBufferPrivate, private FFmpegAppendParserClient {
public:
    static Ref<SourceBufferPrivateFFmpeg> create(MediaSourcePrivateFFmpeg&, const ContentType&, unsigned identifier);
    ~SourceBufferPrivateFFmpeg();

    constexpr MediaPlatformType platformType() const final { return MediaPlatformType::FFmpeg; }

    // MediaPlayerPrivateFFmpegMSE: a decoder queue has room again.
    void provideMoreSamples(TrackID trackID) { provideMediaData(trackID); }
    RefPtr<FFmpegMSETrackFormat> formatForTrack(TrackID) const;

    unsigned identifier() const { return m_identifier; }
    size_t platformMaximumBufferSize() const final;
    bool canSwitchToType(const ContentType&) final;
    void removedFromMediaSource() final;

private:
    SourceBufferPrivateFFmpeg(MediaSourcePrivateFFmpeg&, const ContentType&, unsigned identifier);
    RefPtr<MediaPlayerPrivateFFmpegMSE> player() const;

    // SourceBufferPrivate
    Ref<MediaPromise> appendInternal(Ref<SharedBuffer>&&) final;
    void resetParserStateInternal() final;
    void flush(TrackID) final;
    void enqueueSample(Ref<MediaSample>&&, TrackID) final;
    void allSamplesInTrackEnqueued(TrackID) final;
    bool isReadyForMoreSamples(TrackID) final;
    void notifyClientWhenReadyForMoreSamples(TrackID) final;

    // FFmpegAppendParserClient
    void appendParserDidParseInitializationSegment(const FFmpegParsedInitializationSegment&) final;
    void appendParserDidParseSample(const FFmpegParsedTrack&, AVPacket*) final;

#if !RELEASE_LOG_DISABLED
    const Logger& logger() const final { return m_logger.get(); }
    ASCIILiteral logClassName() const final { return "SourceBufferPrivateFFmpeg"_s; }
    uint64_t logIdentifier() const final { return m_logIdentifier; }
    WTFLogChannel& logChannel() const final;
    const Logger& sourceBufferLogger() const final { return m_logger.get(); }
    uint64_t sourceBufferLogIdentifier() final { return logIdentifier(); }
#endif

    const unsigned m_identifier;
    const String m_mimeType;
    const FFmpegMedia::ByteStreamFormat m_format;
    FFmpegAppendParser m_parser;
    HashMap<TrackID, Ref<FFmpegMSETrackFormat>> m_formats;
    unsigned m_generation { 0 };
    std::atomic<bool> m_hasVideo { false };
    // Console log bookkeeping.
    uint64_t m_bytesAppended { 0 };
    unsigned m_appends { 0 };
    uint64_t m_lastContentSize { 0 };
    double m_appendFirst { 0 };
    double m_appendLast { 0 };
#if !RELEASE_LOG_DISABLED
    const Ref<const Logger> m_logger;
    const uint64_t m_logIdentifier;
#endif
};

} // namespace WebCore

SPECIALIZE_TYPE_TRAITS_BEGIN(WebCore::SourceBufferPrivateFFmpeg)
static bool isType(const WebCore::SourceBufferPrivate& sourceBuffer) { return sourceBuffer.platformType() == WebCore::MediaPlatformType::FFmpeg; }
SPECIALIZE_TYPE_TRAITS_END()

#endif // ENABLE(VIDEO) && USE(FFMPEG) && ENABLE(MEDIA_SOURCE)
