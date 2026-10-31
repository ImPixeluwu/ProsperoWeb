// SPDX-License-Identifier: GPL-3.0-or-later
//
// MSE samples and track descriptions of the ffmpeg media engine. A
// MediaSampleFFmpeg wraps one demuxed AVPacket (shared between copies) with
// the MediaTime timestamps the generic SourceBufferPrivate code reads and
// rewrites (timestampOffset, append windows, sequence mode). Its
// FFmpegMSETrackFormat carries the codec parameters of the initialization
// segment the sample came from, so the player can create decoders and
// notice codec configuration changes (new avcC/esds after a quality switch).

#pragma once

#if ENABLE(VIDEO) && USE(FFMPEG) && ENABLE(MEDIA_SOURCE)

#include "AudioTrackPrivate.h"
#include "FFmpegUtilities.h"
#include "FloatSize.h"
#include "MediaDescription.h"
#include "MediaSample.h"
#include "VideoTrackPrivate.h"
#include <wtf/MediaTime.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/text/WTFString.h>

namespace WebCore {

struct FFmpegParsedTrack;

class FFmpegMSETrackFormat final : public ThreadSafeRefCounted<FFmpegMSETrackFormat> {
public:
    // nullptr when the codec parameters cannot be copied.
    static RefPtr<FFmpegMSETrackFormat> create(const FFmpegParsedTrack&, unsigned generation);

    TrackID trackID() const { return m_trackID; }
    bool isVideo() const { return m_isVideo; }
    const String& fourCC() const { return m_fourCC; }
    const String& codecName() const { return m_codecName; }
    const AVCodecParameters& parameters() const { return *m_parameters; }
    // Display size of a video track (sample aspect ratio applied); empty for
    // audio.
    FloatSize presentationSize() const { return m_presentationSize; }
    // Incremented for every initialization segment of the SourceBuffer.
    unsigned generation() const { return m_generation; }
    bool hasSameDecoderConfiguration(const FFmpegMSETrackFormat&) const;

private:
    FFmpegMSETrackFormat() = default;

    TrackID m_trackID { 0 };
    bool m_isVideo { false };
    String m_fourCC;
    String m_codecName;
    AVCodecParametersPtr m_parameters;
    FloatSize m_presentationSize;
    unsigned m_generation { 0 };
};

class FFmpegMSEPacket final : public ThreadSafeRefCounted<FFmpegMSEPacket> {
public:
    static Ref<FFmpegMSEPacket> create(AVPacketPtr&& packet) { return adoptRef(*new FFmpegMSEPacket(WTF::move(packet))); }
    const AVPacket& packet() const { return *m_packet; }

private:
    explicit FFmpegMSEPacket(AVPacketPtr&& packet)
        : m_packet(WTF::move(packet))
    {
    }
    const AVPacketPtr m_packet;
};

class MediaSampleFFmpeg final : public MediaSample {
public:
    static Ref<MediaSampleFFmpeg> create(Ref<FFmpegMSETrackFormat>&& format, Ref<FFmpegMSEPacket>&& packet, const MediaTime& presentationTime, const MediaTime& decodeTime, const MediaTime& duration, bool isSync)
    {
        return adoptRef(*new MediaSampleFFmpeg(WTF::move(format), WTF::move(packet), presentationTime, decodeTime, duration, isSync ? IsSync : None));
    }

    const AVPacket& packet() const { return m_packet->packet(); }
    const FFmpegMSETrackFormat& format() const { return m_format.get(); }
    Ref<FFmpegMSETrackFormat> protectedFormat() const { return m_format; }

    MediaTime presentationTime() const final { return m_presentationTime; }
    MediaTime decodeTime() const final { return m_decodeTime; }
    MediaTime duration() const final { return m_duration; }
    TrackID trackID() const final { return m_format->trackID(); }
    size_t sizeInBytes() const final { return static_cast<size_t>(std::max(packet().size, 0)) + sizeof(*this) + sizeof(AVPacket); }
    FloatSize presentationSize() const final { return m_format->presentationSize(); }
    void offsetTimestampsBy(const MediaTime& offset) final
    {
        m_presentationTime += offset;
        m_decodeTime += offset;
    }
    void setTimestamps(const MediaTime& presentationTime, const MediaTime& decodeTime) final
    {
        m_presentationTime = presentationTime;
        m_decodeTime = decodeTime;
    }
    Ref<MediaSample> createNonDisplayingCopy() const final
    {
        return adoptRef(*new MediaSampleFFmpeg(m_format.copyRef(), m_packet.copyRef(), m_presentationTime, m_decodeTime, m_duration, static_cast<SampleFlags>(m_flags | IsNonDisplaying)));
    }
    Ref<MediaSample> createCopyWithAdjustedStartTime(const MediaTime& offset) const final
    {
        MediaTime clamped = std::max(MediaTime::zeroTime(), std::min(offset, m_duration));
        return adoptRef(*new MediaSampleFFmpeg(m_format.copyRef(), m_packet.copyRef(), m_presentationTime + clamped, m_decodeTime + clamped, m_duration - clamped, m_flags));
    }
    SampleFlags flags() const final { return m_flags; }
    PlatformSample platformSample() const final { return PlatformSample { static_cast<const MockSampleBox*>(nullptr) }; }
    Type type() const final { return Type::None; }

private:
    MediaSampleFFmpeg(Ref<FFmpegMSETrackFormat>&& format, Ref<FFmpegMSEPacket>&& packet, const MediaTime& presentationTime, const MediaTime& decodeTime, const MediaTime& duration, SampleFlags flags)
        : m_format(WTF::move(format))
        , m_packet(WTF::move(packet))
        , m_presentationTime(presentationTime)
        , m_decodeTime(decodeTime)
        , m_duration(duration)
        , m_flags(flags)
    {
    }

    const Ref<FFmpegMSETrackFormat> m_format;
    const Ref<FFmpegMSEPacket> m_packet;
    MediaTime m_presentationTime;
    MediaTime m_decodeTime;
    MediaTime m_duration;
    const SampleFlags m_flags;
};

class FFmpegMSEMediaDescription final : public MediaDescription {
public:
    static Ref<FFmpegMSEMediaDescription> create(const String& fourCC, bool isVideo) { return adoptRef(*new FFmpegMSEMediaDescription(fourCC, isVideo)); }

    bool isVideo() const final { return m_isVideo; }
    bool isAudio() const final { return !m_isVideo; }
    bool isText() const final { return false; }

private:
    FFmpegMSEMediaDescription(const String& fourCC, bool isVideo)
        : MediaDescription(fourCC.isolatedCopy())
        , m_isVideo(isVideo)
    {
    }
    const bool m_isVideo;
};

class FFmpegMSEVideoTrackPrivate final : public VideoTrackPrivate {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(FFmpegMSEVideoTrackPrivate);
public:
    static Ref<FFmpegMSEVideoTrackPrivate> create(TrackID id) { return adoptRef(*new FFmpegMSEVideoTrackPrivate(id)); }
    TrackID id() const final { return m_id; }
    Kind kind() const final { return Kind::Main; }

private:
    explicit FFmpegMSEVideoTrackPrivate(TrackID id)
        : m_id(id)
    {
    }
    const TrackID m_id;
};

class FFmpegMSEAudioTrackPrivate final : public AudioTrackPrivate {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(FFmpegMSEAudioTrackPrivate);
public:
    static Ref<FFmpegMSEAudioTrackPrivate> create(TrackID id) { return adoptRef(*new FFmpegMSEAudioTrackPrivate(id)); }
    TrackID id() const final { return m_id; }
    Kind kind() const final { return Kind::Main; }

private:
    explicit FFmpegMSEAudioTrackPrivate(TrackID id)
        : m_id(id)
    {
    }
    const TrackID m_id;
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG) && ENABLE(MEDIA_SOURCE)
