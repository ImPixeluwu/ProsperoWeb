// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "SourceBufferPrivateFFmpeg.h"

#if ENABLE(VIDEO) && USE(FFMPEG) && ENABLE(MEDIA_SOURCE)

#include "ContentType.h"
#include "FFmpegLog.h"
#include "FFmpegMSEPolicy.h"
#include "FFmpegSupportTable.h"
#include "Logging.h"
#include "MediaPlayerPrivateFFmpegMSE.h"
#include "MediaSampleFFmpeg.h"
#include "MediaSourcePrivateFFmpeg.h"
#include "SharedBuffer.h"
#include "SourceBufferPrivateClient.h"
#include <wtf/NativePromise.h>
#include <wtf/text/StringBuilder.h>

namespace WebCore {

namespace {

FFmpegMedia::ByteStreamFormat byteStreamFormat(const ContentType& contentType)
{
    auto container = FFmpegMedia::mediaSourceContainerForMIMEType(contentType.containerType().utf8().toStdString());
    return container == FFmpegMedia::Container::WebM ? FFmpegMedia::ByteStreamFormat::WebM : FFmpegMedia::ByteStreamFormat::MP4;
}

MediaTime mediaTime(int64_t timestamp, AVRational timeBase)
{
    auto value = FFmpegMedia::timeValueFromTimestamp(timestamp, timeBase.num, timeBase.den);
    if (!value.valid)
        return MediaTime::createWithDouble(timestamp * av_q2d(timeBase));
    return MediaTime(value.value, value.timescale);
}

} // namespace

// MARK: - FFmpegMSETrackFormat

RefPtr<FFmpegMSETrackFormat> FFmpegMSETrackFormat::create(const FFmpegParsedTrack& track, unsigned generation)
{
    if (!track.parameters)
        return nullptr;
    RefPtr format = adoptRef(*new FFmpegMSETrackFormat);
    format->m_parameters.reset(avcodec_parameters_alloc());
    if (!format->m_parameters || avcodec_parameters_copy(format->m_parameters.get(), track.parameters) < 0)
        return nullptr;
    format->m_trackID = track.trackID;
    format->m_isVideo = track.type == AVMEDIA_TYPE_VIDEO;
    format->m_fourCC = String::fromLatin1(track.fourCC.c_str());
    format->m_codecName = String::fromLatin1(track.codecName.c_str());
    format->m_generation = generation;
    if (format->m_isVideo) {
        double width = track.parameters->width;
        double height = track.parameters->height;
        AVRational sar = track.sampleAspectRatio;
        if (sar.num > 0 && sar.den > 0)
            width *= av_q2d(sar);
        // An unknown size still marks the sample as video (the generic code
        // tells video tracks by a non-empty presentation size).
        format->m_presentationSize = FloatSize(width > 0 ? width : 1, height > 0 ? height : 1);
    }
    return format;
}

bool FFmpegMSETrackFormat::hasSameDecoderConfiguration(const FFmpegMSETrackFormat& other) const
{
    auto& a = parameters();
    auto& b = other.parameters();
    if (a.codec_id != b.codec_id || a.extradata_size != b.extradata_size)
        return false;
    return !a.extradata_size || !memcmp(a.extradata, b.extradata, a.extradata_size);
}

// MARK: - SourceBufferPrivateFFmpeg

Ref<SourceBufferPrivateFFmpeg> SourceBufferPrivateFFmpeg::create(MediaSourcePrivateFFmpeg& parent, const ContentType& contentType, unsigned identifier)
{
    return adoptRef(*new SourceBufferPrivateFFmpeg(parent, contentType, identifier));
}

SourceBufferPrivateFFmpeg::SourceBufferPrivateFFmpeg(MediaSourcePrivateFFmpeg& parent, const ContentType& contentType, unsigned identifier)
    : SourceBufferPrivate(parent)
    , m_identifier(identifier)
    , m_mimeType(contentType.containerType().convertToASCIILowercase())
    , m_format(byteStreamFormat(contentType))
    , m_parser(m_format)
#if !RELEASE_LOG_DISABLED
    , m_logger(parent.platformPlayer()->mediaPlayerLogger())
    , m_logIdentifier(childLogIdentifier(parent.platformPlayer()->mediaPlayerLogIdentifier(), identifier))
#endif
{
    // Until an initialization segment says otherwise, a video/* buffer gets
    // the video quota.
    m_hasVideo = m_mimeType.startsWith("video/"_s);
}

SourceBufferPrivateFFmpeg::~SourceBufferPrivateFFmpeg() = default;

RefPtr<MediaPlayerPrivateFFmpegMSE> SourceBufferPrivateFFmpeg::player() const
{
    RefPtr mediaSource = m_mediaSource.get();
    if (!mediaSource || mediaSource->platformType() != MediaPlatformType::FFmpeg)
        return nullptr;
    return static_cast<MediaSourcePrivateFFmpeg&>(*mediaSource).platformPlayer();
}

RefPtr<FFmpegMSETrackFormat> SourceBufferPrivateFFmpeg::formatForTrack(TrackID trackID) const
{
    auto it = m_formats.find(trackID);
    if (it == m_formats.end())
        return nullptr;
    return it->value.ptr();
}

size_t SourceBufferPrivateFFmpeg::platformMaximumBufferSize() const
{
    return FFmpegMedia::mseBufferQuota(m_hasVideo);
}

bool SourceBufferPrivateFFmpeg::canSwitchToType(const ContentType& contentType)
{
    // changeType() within the same byte stream format (e.g. avc1 -> vp09 in
    // MP4); switching between MP4 and WebM would need a new parser.
    MediaEngineSupportParameters parameters {
        .platformType = PlatformMediaDecodingType::MediaSource,
        .type = contentType
    };
    if (MediaPlayerPrivateFFmpegMSE::supportsType(parameters) == MediaPlayer::SupportsType::IsNotSupported)
        return false;
    return byteStreamFormat(contentType) == m_format;
}

void SourceBufferPrivateFFmpeg::removedFromMediaSource()
{
    if (RefPtr player = this->player())
        player->sourceBufferRemoved(*this);
    SourceBufferPrivate::removedFromMediaSource();
}

Ref<MediaPromise> SourceBufferPrivateFFmpeg::appendInternal(Ref<SharedBuffer>&& data)
{
    auto bytes = data->span();
    ++m_appends;
    m_bytesAppended += bytes.size();
    m_appendFirst = std::numeric_limits<double>::infinity();
    m_appendLast = -std::numeric_limits<double>::infinity();

    uint64_t contentSize = this->contentSize();
    if (contentSize + (1u << 20) < m_lastContentSize)
        ffmpegMediaLog("mse: #%u evicted: buffer %llu KiB -> %llu KiB", m_identifier, static_cast<unsigned long long>(m_lastContentSize >> 10), static_cast<unsigned long long>(contentSize >> 10));
    m_lastContentSize = contentSize;

    if (RefPtr player = this->player())
        player->didAppendBytes(bytes.size());

    bool ok = m_parser.append(bytes.data(), bytes.size(), *this);
    if (!ok) {
        ffmpegMediaLog("mse: #%u append %zu bytes failed: %s", m_identifier, bytes.size(), m_parser.error().c_str());
        return MediaPromise::createAndReject(PlatformMediaError::ParsingError);
    }

    unsigned samples = m_parser.lastAppendSamples();
    // Each append once while starting, then every 20th (YouTube appends a
    // few times per second).
    if (m_appends <= 8 || !(m_appends % 20) || !samples) {
        if (samples)
            ffmpegMediaLog("mse: #%u append %zu bytes -> %u samples (%.3f-%.3f), buffer %llu KiB of %zu KiB, %llu KiB appended in %u appends",
                m_identifier, bytes.size(), samples, m_appendFirst, m_appendLast, static_cast<unsigned long long>(contentSize >> 10),
                platformMaximumBufferSize() >> 10, static_cast<unsigned long long>(m_bytesAppended >> 10), m_appends);
        else
            ffmpegMediaLog("mse: #%u append %zu bytes -> no samples (%zu bytes pending)", m_identifier, bytes.size(), m_parser.pendingBytes());
    }
    return MediaPromise::createAndResolve();
}

void SourceBufferPrivateFFmpeg::resetParserStateInternal()
{
    ffmpegMediaLog("mse: #%u resetParserState (%zu bytes pending dropped)", m_identifier, m_parser.pendingBytes());
    m_parser.reset();
}

void SourceBufferPrivateFFmpeg::appendParserDidParseInitializationSegment(const FFmpegParsedInitializationSegment& parsed)
{
    ++m_generation;
    InitializationSegment segment;
    segment.duration = std::isfinite(parsed.durationSeconds) ? MediaTime::createWithDouble(parsed.durationSeconds) : MediaTime::invalidTime();

    StringBuilder description;
    bool hasVideo = false;
    for (auto& track : parsed.tracks) {
        RefPtr format = FFmpegMSETrackFormat::create(track, m_generation);
        if (!format)
            continue;
        auto& parameters = format->parameters();
        if (!description.isEmpty())
            description.append(", "_s);
        if (format->isVideo()) {
            hasVideo = true;
            description.append("video "_s, track.trackID, ' ', format->codecName(), ' ', parameters.width, 'x', parameters.height);
            SourceBufferPrivateClient::InitializationSegment::VideoTrackInformation info;
            info.track = FFmpegMSEVideoTrackPrivate::create(track.trackID);
            info.description = FFmpegMSEMediaDescription::create(format->fourCC(), true);
            segment.videoTracks.append(WTF::move(info));
        } else {
            description.append("audio "_s, track.trackID, ' ', format->codecName(), ' ', parameters.sample_rate, " Hz "_s, parameters.ch_layout.nb_channels, "ch"_s);
            SourceBufferPrivateClient::InitializationSegment::AudioTrackInformation info;
            info.track = FFmpegMSEAudioTrackPrivate::create(track.trackID);
            info.description = FFmpegMSEMediaDescription::create(format->fourCC(), false);
            segment.audioTracks.append(WTF::move(info));
        }
        m_formats.set(track.trackID, format.releaseNonNull());
    }
    m_hasVideo = hasVideo;
    ffmpegMediaLog("mse: #%u init segment #%u (%s): %s", m_identifier, m_generation, m_mimeType.utf8().legacyCStringPointer(), description.toString().utf8().legacyCStringPointer());

    if (RefPtr player = this->player()) {
        for (auto& format : m_formats.values()) {
            if (format->generation() == m_generation && format->isVideo())
                player->videoFormatAvailable(format.get());
        }
    }
    didReceiveInitializationSegment(WTF::move(segment));
}

void SourceBufferPrivateFFmpeg::appendParserDidParseSample(const FFmpegParsedTrack& track, AVPacket* packet)
{
    auto it = m_formats.find(track.trackID);
    if (it == m_formats.end())
        return;
    AVPacketPtr owned(av_packet_alloc());
    if (!owned)
        return;
    av_packet_move_ref(owned.get(), packet);
    auto presentationTime = mediaTime(owned->pts, track.timeBase);
    auto decodeTime = mediaTime(owned->dts, track.timeBase);
    auto duration = mediaTime(owned->duration, track.timeBase);
    bool isSync = owned->flags & AV_PKT_FLAG_KEY;
    m_appendFirst = std::min(m_appendFirst, presentationTime.toDouble());
    m_appendLast = std::max(m_appendLast, (presentationTime + duration).toDouble());
    didReceiveSample(MediaSampleFFmpeg::create(it->value.copyRef(), FFmpegMSEPacket::create(WTF::move(owned)), presentationTime, decodeTime, duration, isSync));
}

void SourceBufferPrivateFFmpeg::flush(TrackID trackID)
{
    if (RefPtr player = this->player())
        player->flushTrack(*this, trackID);
}

void SourceBufferPrivateFFmpeg::enqueueSample(Ref<MediaSample>&& sample, TrackID trackID)
{
    if (sample->type() != MediaSample::Type::None)
        return;
    if (RefPtr player = this->player())
        player->enqueueSample(*this, trackID, static_cast<MediaSampleFFmpeg&>(sample.get()));
}

void SourceBufferPrivateFFmpeg::allSamplesInTrackEnqueued(TrackID trackID)
{
    if (RefPtr player = this->player())
        player->trackEnded(*this, trackID);
}

bool SourceBufferPrivateFFmpeg::isReadyForMoreSamples(TrackID trackID)
{
    RefPtr player = this->player();
    return !player || player->isReadyForMoreSamples(*this, trackID);
}

void SourceBufferPrivateFFmpeg::notifyClientWhenReadyForMoreSamples(TrackID trackID)
{
    if (RefPtr player = this->player())
        player->notifyWhenReadyForMoreSamples(*this, trackID);
}

#if !RELEASE_LOG_DISABLED
WTFLogChannel& SourceBufferPrivateFFmpeg::logChannel() const
{
    return LogMediaSource;
}
#endif

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG) && ENABLE(MEDIA_SOURCE)
