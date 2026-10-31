// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "MediaPlayerPrivateFFmpegMSE.h"

#if ENABLE(VIDEO) && USE(FFMPEG) && ENABLE(MEDIA_SOURCE)

#include "AudioSink.h"
#include "FFmpegAudioDecoder.h"
#include "FFmpegDemuxer.h"
#include "FFmpegLog.h"
#include "FFmpegMSEParsing.h"
#include "FFmpegMSEPolicy.h"
#include "FFmpegSupportTable.h"
#include "FFmpegVideoDecoder.h"
#include "FFmpegVideoOverlay.h"
#include "GraphicsContext.h"
#include "MediaPlayer.h"
#include "MediaPlayerPrivateFFmpeg.h"
#include "MediaSampleFFmpeg.h"
#include "MediaSourcePrivateClient.h"
#include "MediaSourcePrivateFFmpeg.h"
#include "NativeImage.h"
#include "SourceBufferPrivateFFmpeg.h"
#include "VideoPlaybackQualityMetrics.h"
#include <wtf/MonotonicTime.h>
#include <wtf/NativePromise.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringConcatenateNumbers.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(MediaPlayerPrivateFFmpegMSE);

namespace {

constexpr double audioWatchdogSeconds = 2.0;
constexpr double statsIntervalSeconds = 5.0;
// A seek whose first frame never shows (decode failure) still completes.
constexpr double seekCompletionTimeout = 3.0;
constexpr AVRational queueTimeBase { 1, 1000000 };

double monotonicNow()
{
    return MonotonicTime::now().secondsSinceEpoch().seconds();
}

const char* readyStateName(MediaPlayer::ReadyState state)
{
    switch (state) {
    case MediaPlayer::ReadyState::HaveNothing: return "HaveNothing";
    case MediaPlayer::ReadyState::HaveMetadata: return "HaveMetadata";
    case MediaPlayer::ReadyState::HaveCurrentData: return "HaveCurrentData";
    case MediaPlayer::ReadyState::HaveFutureData: return "HaveFutureData";
    case MediaPlayer::ReadyState::HaveEnoughData: return "HaveEnoughData";
    }
    return "?";
}

FFmpegMedia::ReadyLevel toReadyLevel(MediaPlayer::ReadyState state)
{
    switch (state) {
    case MediaPlayer::ReadyState::HaveNothing: return FFmpegMedia::ReadyLevel::HaveNothing;
    case MediaPlayer::ReadyState::HaveMetadata: return FFmpegMedia::ReadyLevel::HaveMetadata;
    case MediaPlayer::ReadyState::HaveCurrentData: return FFmpegMedia::ReadyLevel::HaveCurrentData;
    case MediaPlayer::ReadyState::HaveFutureData: return FFmpegMedia::ReadyLevel::HaveFutureData;
    case MediaPlayer::ReadyState::HaveEnoughData: return FFmpegMedia::ReadyLevel::HaveEnoughData;
    }
    return FFmpegMedia::ReadyLevel::HaveNothing;
}

MediaPlayer::ReadyState toReadyState(FFmpegMedia::ReadyLevel level)
{
    switch (level) {
    case FFmpegMedia::ReadyLevel::HaveNothing: return MediaPlayer::ReadyState::HaveNothing;
    case FFmpegMedia::ReadyLevel::HaveMetadata: return MediaPlayer::ReadyState::HaveMetadata;
    case FFmpegMedia::ReadyLevel::HaveCurrentData: return MediaPlayer::ReadyState::HaveCurrentData;
    case FFmpegMedia::ReadyLevel::HaveFutureData: return MediaPlayer::ReadyState::HaveFutureData;
    case FFmpegMedia::ReadyLevel::HaveEnoughData: return MediaPlayer::ReadyState::HaveEnoughData;
    }
    return MediaPlayer::ReadyState::HaveNothing;
}

int64_t toQueueTimestamp(const MediaTime& time)
{
    if (!time.isValid())
        return AV_NOPTS_VALUE;
    if (time.hasDoubleValue() || time.isIndefinite() || time.isPositiveInfinite() || time.isNegativeInfinite())
        return FFmpegMedia::microsecondsFromSeconds(time.toDouble());
    return FFmpegMedia::microsecondsFromTimeValue(time.timeValue(), time.timeScale());
}

FFmpegMedia::TimeRangeSet toRangeSet(const PlatformTimeRanges& ranges)
{
    FFmpegMedia::TimeRangeSet set;
    for (unsigned i = 0; i < ranges.length(); ++i)
        set.add(ranges.start(i).toDouble(), ranges.end(i).toDouble());
    return set;
}

} // namespace

// MARK: - Registration

class MediaPlayerFactoryFFmpegMSE final : public MediaPlayerFactory {
    WTF_MAKE_TZONE_ALLOCATED(MediaPlayerFactoryFFmpegMSE);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(MediaPlayerFactoryFFmpegMSE);
private:
    MediaPlayerEnums::MediaEngineIdentifier identifier() const final { return MediaPlayerEnums::MediaEngineIdentifier::FFmpegMSE; }

    Ref<MediaPlayerPrivateInterface> createMediaEnginePlayer(MediaPlayer& player) const final
    {
        return adoptRef(*new MediaPlayerPrivateFFmpegMSE(player));
    }

    void getSupportedTypes(HashSet<String>& types) const final
    {
        MediaPlayerPrivateFFmpegMSE::getSupportedTypes(types);
    }

    MediaPlayer::SupportsType supportsTypeAndCodecs(const MediaEngineSupportParameters& parameters) const final
    {
        return MediaPlayerPrivateFFmpegMSE::supportsType(parameters);
    }
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(MediaPlayerFactoryFFmpegMSE);

void MediaPlayerPrivateFFmpegMSE::registerMediaEngine(MediaEngineRegistrar registrar)
{
    registrar(makeUnique<MediaPlayerFactoryFFmpegMSE>());
}

void MediaPlayerPrivateFFmpegMSE::getSupportedTypes(HashSet<String>& types)
{
    for (auto type : { "video/mp4"_s, "audio/mp4"_s, "video/webm"_s, "audio/webm"_s })
        types.add(type);
}

MediaPlayer::SupportsType MediaPlayerPrivateFFmpegMSE::supportsType(const MediaEngineSupportParameters& parameters)
{
    if (parameters.platformType != PlatformMediaDecodingType::MediaSource)
        return MediaPlayer::SupportsType::IsNotSupported;
    auto result = MediaPlayer::SupportsType::IsNotSupported;
    switch (FFmpegMedia::evaluateMediaSource(parameters.type.raw().utf8().toStdString(), MediaPlayerPrivateFFmpeg::decoderAvailability(), MediaPlayerPrivateFFmpeg::videoLimits())) {
    case FFmpegMedia::Support::IsSupported:
        result = MediaPlayer::SupportsType::IsSupported;
        break;
    case FFmpegMedia::Support::MaybeSupported:
        result = MediaPlayer::SupportsType::MayBeSupported;
        break;
    case FFmpegMedia::Support::NotSupported:
        break;
    }
    if (!parameters.type.raw().isEmpty())
        MediaPlayerPrivateFFmpeg::logSupportsType(parameters, result);
    return result;
}

// MARK: - Lifetime

MediaPlayerPrivateFFmpegMSE::MediaPlayerPrivateFFmpegMSE(MediaPlayer& player)
    : m_player(player)
    , m_tickTimer(RunLoop::mainSingleton(), "MediaPlayerPrivateFFmpegMSE::Tick"_s, this, &MediaPlayerPrivateFFmpegMSE::tick)
    , m_overlayTimer(RunLoop::mainSingleton(), "MediaPlayerPrivateFFmpegMSE::OverlayTick"_s, this, &MediaPlayerPrivateFFmpegMSE::overlayTick)
    , m_clock(FFmpegPlaybackClock::create())
{
    m_video.isVideo = true;
}

MediaPlayerPrivateFFmpegMSE::~MediaPlayerPrivateFFmpegMSE()
{
    stopPipelines();
    if (RefPtr mediaSource = std::exchange(m_mediaSourcePrivate, nullptr))
        mediaSource->setPlayer(nullptr);
}

void MediaPlayerPrivateFFmpegMSE::stopPipelines()
{
    m_tickTimer.stop();
    m_overlayTimer.stop();
    // The presenter reads the video decoder and the overlay, the sink calls
    // into the audio decoder, the decoders read the queues.
    stopPresenter();
    setAudioSink(nullptr);
    m_clock->setAudioDecoder(nullptr);
    for (auto* pipeline : { &m_video, &m_audio }) {
        if (pipeline->queue)
            pipeline->queue->abort();
    }
    if (RefPtr decoder = std::exchange(m_videoDecoder, nullptr))
        decoder->stop();
    if (RefPtr decoder = std::exchange(m_audioDecoder, nullptr))
        decoder->stop();
    // The hole must not outlive the overlay.
    if (m_overlay && m_overlay->punchesHole()) {
        if (RefPtr player = m_player.get())
            player->repaint();
    }
    m_overlay = nullptr;
    m_overlayCreated = false;
    for (auto* pipeline : { &m_video, &m_audio }) {
        bool isVideo = pipeline->isVideo;
        *pipeline = Pipeline { };
        pipeline->isVideo = isVideo;
    }
    if (auto promise = std::exchange(m_seekPromise, std::nullopt))
        promise->reject(PlatformMediaError::Cancelled);
    m_seeking = false;
}

// MARK: - Loading

void MediaPlayerPrivateFFmpegMSE::load(const String&)
{
    // Only reached for non-MediaSource loads, which supportsType() refuses.
    setNetworkState(MediaPlayer::NetworkState::FormatError);
}

void MediaPlayerPrivateFFmpegMSE::load(const URL&, const LoadOptions&, MediaSourcePrivateClient& client)
{
    stopPipelines();
    m_retainedFrame = nullptr;
    m_imageFrame = nullptr;
    m_currentImage = nullptr;
    m_ended = false;
    if (RefPtr existing = client.mediaSourcePrivate(); existing && existing->platformType() == MediaPlatformType::FFmpeg) {
        Ref mediaSource = downcast<MediaSourcePrivateFFmpeg>(*existing);
        mediaSource->setPlayer(this);
        m_mediaSourcePrivate = WTF::move(mediaSource);
        client.reOpen();
    } else
        m_mediaSourcePrivate = MediaSourcePrivateFFmpeg::open(client, *this);
    ffmpegMediaLog("mse: load (MediaSource)");
    m_clock->reset(0, monotonicNow());
    m_clock->setRate(m_rate, monotonicNow());
    setNetworkState(MediaPlayer::NetworkState::Loading);
    scheduleTick(0_s);
}

#if ENABLE(MEDIA_STREAM)
void MediaPlayerPrivateFFmpegMSE::load(MediaStreamPrivate&)
{
    setNetworkState(MediaPlayer::NetworkState::FormatError);
}
#endif

void MediaPlayerPrivateFFmpegMSE::cancelLoad()
{
    ffmpegMediaLog("mse: cancelLoad");
    stopPipelines();
    setNetworkState(MediaPlayer::NetworkState::Idle);
}

// MARK: - Sample input

MediaPlayerPrivateFFmpegMSE::Pipeline* MediaPlayerPrivateFFmpegMSE::pipelineFor(SourceBufferPrivateFFmpeg& sourceBuffer, TrackID trackID)
{
    if (m_video.matches(sourceBuffer, trackID))
        return &m_video;
    if (m_audio.matches(sourceBuffer, trackID))
        return &m_audio;
    return nullptr;
}

bool MediaPlayerPrivateFFmpegMSE::createPipeline(Pipeline& pipeline, SourceBufferPrivateFFmpeg& sourceBuffer, TrackID trackID, const FFmpegMSETrackFormat& format)
{
    pipeline.sourceBuffer = sourceBuffer;
    pipeline.sourceBufferKey = &sourceBuffer;
    pipeline.trackID = trackID;
    pipeline.decoderFormat = const_cast<FFmpegMSETrackFormat*>(&format);
    pipeline.queue = std::make_unique<FFmpegPacketQueue>();
    pipeline.queue->setTimeBase(queueTimeBase);
    pipeline.queue->setEnabled(true);

    FFmpegStreamInfo stream;
    stream.index = 0;
    stream.timeBase = queueTimeBase;
    stream.startTime = 0;
    stream.parameters.reset(avcodec_parameters_alloc());
    if (!stream.parameters || avcodec_parameters_copy(stream.parameters.get(), &format.parameters()) < 0) {
        pipeline.failed = true;
        return false;
    }

    double position = playbackPosition();
    auto codec = format.codecName().utf8();
    if (pipeline.isVideo) {
        m_videoDecoder = FFmpegVideoDecoder::create(stream, *pipeline.queue);
        if (!m_videoDecoder)
            pipeline.failed = true;
        else {
            RefPtr decoder = m_videoDecoder;
            decoder->setSuspended(!m_pageVisible);
            decoder->setSeekTarget(position, pipeline.serial);
            decoder->start();
            startPresenter();
        }
    } else {
        m_audioDecoder = FFmpegAudioDecoder::create(stream, *pipeline.queue, AudioSink::sampleRate, AudioSink::channels);
        if (!m_audioDecoder)
            pipeline.failed = true;
        else {
            RefPtr decoder = m_audioDecoder;
            decoder->setSeekTarget(position, pipeline.serial);
            decoder->setDiscard(m_rate != 1);
            decoder->start();
            m_clock->setAudioDecoder(decoder.copyRef());
            m_clock->setAudioSerial(pipeline.serial);
            createAudioSink(false);
            applyGain();
        }
    }
    ffmpegMediaLog("mse: %s pipeline: SourceBuffer #%u track %llu %s%s", pipeline.isVideo ? "video" : "audio", sourceBuffer.identifier(),
        static_cast<unsigned long long>(trackID), codec.legacyCStringPointer(), pipeline.failed ? ": NO DECODER" : "");
    if (pipeline.failed) {
        if (RefPtr mediaSource = m_mediaSourcePrivate; mediaSource) {
            if (RefPtr client = mediaSource->client())
                client->failedToCreateRenderer(pipeline.isVideo ? MediaSourcePrivateClient::RendererType::Video : MediaSourcePrivateClient::RendererType::Audio);
        }
        return false;
    }
    updatePlaybackRunning();
    scheduleTick(0_s);
    return true;
}

void MediaPlayerPrivateFFmpegMSE::enqueueSample(SourceBufferPrivateFFmpeg& sourceBuffer, TrackID trackID, MediaSampleFFmpeg& sample)
{
    auto& format = sample.format();
    Pipeline& pipeline = format.isVideo() ? m_video : m_audio;
    if (!pipeline.bound() && !createPipeline(pipeline, sourceBuffer, trackID, format))
        return;
    if (!pipeline.matches(sourceBuffer, trackID) || pipeline.failed || !pipeline.queue)
        return;

    AVPacketPtr packet(av_packet_alloc());
    if (!packet || av_packet_ref(packet.get(), &sample.packet()) < 0)
        return;
    packet->pts = toQueueTimestamp(sample.presentationTime());
    packet->dts = toQueueTimestamp(sample.decodeTime());
    packet->duration = std::max<int64_t>(0, toQueueTimestamp(sample.duration()));
    packet->time_base = queueTimeBase;
    packet->stream_index = 0;
    if (sample.isNonDisplaying())
        packet->flags |= AV_PKT_FLAG_DISCARD;

    // A new initialization segment (YouTube quality switch) brings a new
    // avcC/esds: h264 and aac decoders take it as packet side data.
    RefPtr decoderFormat = pipeline.decoderFormat;
    if (!decoderFormat || decoderFormat.get() != &format) {
        bool same = decoderFormat && decoderFormat->hasSameDecoderConfiguration(format);
        if (!same) {
            auto& parameters = format.parameters();
            if (decoderFormat && decoderFormat->parameters().codec_id != parameters.codec_id)
                ffmpegMediaLog("mse: %s codec change %s -> %s is not supported", pipeline.isVideo ? "video" : "audio",
                    decoderFormat->codecName().utf8().legacyCStringPointer(), format.codecName().utf8().legacyCStringPointer());
            else if (parameters.extradata_size > 0) {
                if (auto* data = av_packet_new_side_data(packet.get(), AV_PKT_DATA_NEW_EXTRADATA, parameters.extradata_size))
                    memcpy(data, parameters.extradata, parameters.extradata_size);
                // No decoder format: resent after a flush, not a change.
                if (decoderFormat)
                    ffmpegMediaLog("mse: %s configuration change (init segment #%u, %dx%d)", pipeline.isVideo ? "video" : "audio",
                        format.generation(), parameters.width, parameters.height);
            }
        }
        pipeline.decoderFormat = const_cast<FFmpegMSETrackFormat*>(&format);
    }

    double end = (sample.presentationTime() + sample.duration()).toDouble();
    pipeline.queue->push(WTF::move(packet), pipeline.serial);
    ++pipeline.enqueued;
    pipeline.endOfStream = false;
    pipeline.lastEnqueuedEnd = std::max(pipeline.lastEnqueuedEnd, end);
}

bool MediaPlayerPrivateFFmpegMSE::isReadyForMoreSamples(SourceBufferPrivateFFmpeg& sourceBuffer, TrackID trackID)
{
    RefPtr format = sourceBuffer.formatForTrack(trackID);
    if (!format)
        return true;
    Pipeline& pipeline = format->isVideo() ? m_video : m_audio;
    // Unbound: the first sample creates the pipeline. Another track's
    // samples are dropped, so it may push as many as it likes.
    if (!pipeline.bound() || !pipeline.matches(sourceBuffer, trackID) || pipeline.failed || !pipeline.queue)
        return true;
    return FFmpegMedia::mseQueueAcceptsMore(pipeline.queue->level());
}

void MediaPlayerPrivateFFmpegMSE::notifyWhenReadyForMoreSamples(SourceBufferPrivateFFmpeg& sourceBuffer, TrackID trackID)
{
    if (auto* pipeline = pipelineFor(sourceBuffer, trackID)) {
        pipeline->wantsMore = true;
        scheduleTick(20_ms);
    }
}

void MediaPlayerPrivateFFmpegMSE::flushPipeline(Pipeline& pipeline, double target)
{
    if (!pipeline.queue)
        return;
    ++pipeline.serial;
    // The presenter first (it must not peek the decoder with the old serial
    // once frames of the new one exist), then the decoder, so the first
    // packets of the new serial are trimmed.
    if (pipeline.isVideo) {
        if (RefPtr presenter = m_presenter)
            presenter->setSerial(pipeline.serial);
        if (RefPtr decoder = m_videoDecoder)
            decoder->setSeekTarget(target, pipeline.serial);
    } else {
        m_clock->setAudioSerial(pipeline.serial);
        if (RefPtr decoder = m_audioDecoder)
            decoder->setSeekTarget(target, pipeline.serial);
        if (m_audioSink)
            m_audioSink->flush();
    }
    pipeline.queue->flush(pipeline.serial);
    pipeline.endOfStream = false;
    pipeline.wantsMore = false;
    pipeline.lastEnqueuedEnd = 0;
    // The decoder may not have seen the configuration of the packets just
    // dropped: resend it with the next one.
    pipeline.decoderFormat = nullptr;
}

void MediaPlayerPrivateFFmpegMSE::flushTrack(SourceBufferPrivateFFmpeg& sourceBuffer, TrackID trackID)
{
    if (auto* pipeline = pipelineFor(sourceBuffer, trackID))
        flushPipeline(*pipeline, playbackPosition());
}

void MediaPlayerPrivateFFmpegMSE::trackEnded(SourceBufferPrivateFFmpeg& sourceBuffer, TrackID trackID)
{
    auto* pipeline = pipelineFor(sourceBuffer, trackID);
    if (!pipeline || !pipeline->queue || pipeline->endOfStream)
        return;
    ffmpegMediaLog("mse: %s track ended after %llu samples (last %.3f)", pipeline->isVideo ? "video" : "audio",
        static_cast<unsigned long long>(pipeline->enqueued), pipeline->lastEnqueuedEnd);
    pipeline->queue->pushEndOfStream(pipeline->serial);
    pipeline->endOfStream = true;
    scheduleTick(0_s);
}

void MediaPlayerPrivateFFmpegMSE::sourceBufferRemoved(SourceBufferPrivateFFmpeg& sourceBuffer)
{
    for (auto* pipeline : { &m_video, &m_audio }) {
        if (pipeline->sourceBufferKey != &sourceBuffer)
            continue;
        ffmpegMediaLog("mse: SourceBuffer #%u removed, stopping its %s pipeline", sourceBuffer.identifier(), pipeline->isVideo ? "video" : "audio");
        if (pipeline->queue)
            pipeline->queue->abort();
        if (pipeline->isVideo) {
            stopPresenter();
            if (RefPtr decoder = std::exchange(m_videoDecoder, nullptr))
                decoder->stop();
        } else {
            setAudioSink(nullptr);
            m_clock->setAudioDecoder(nullptr);
            if (RefPtr decoder = std::exchange(m_audioDecoder, nullptr))
                decoder->stop();
        }
        bool isVideo = pipeline->isVideo;
        *pipeline = Pipeline { };
        pipeline->isVideo = isVideo;
    }
}

void MediaPlayerPrivateFFmpegMSE::videoFormatAvailable(const FFmpegMSETrackFormat& format)
{
    auto size = format.presentationSize();
    if (currentFrame() || size.isEmpty() || size == m_naturalSize)
        return;
    m_naturalSize = size;
    if (RefPtr player = m_player.get())
        player->sizeChanged();
}

void MediaPlayerPrivateFFmpegMSE::didAppendBytes(size_t bytes)
{
    m_bytesAppended += bytes;
}

void MediaPlayerPrivateFFmpegMSE::durationChangedFromMediaSource()
{
    if (RefPtr player = m_player.get())
        player->durationChanged();
}

void MediaPlayerPrivateFFmpegMSE::endOfStreamCleared()
{
    m_ended = false;
}

void MediaPlayerPrivateFFmpegMSE::activeSourceBuffersChanged()
{
    if (RefPtr player = m_player.get())
        player->activeSourceBuffersChanged();
}

// MARK: - Playback control

void MediaPlayerPrivateFFmpegMSE::createAudioSink(bool forceNull)
{
    RefPtr decoder = m_audioDecoder;
    if (!decoder)
        return;
    setAudioSink(nullptr);
    auto render = [decoder](std::span<float> out) {
        decoder->render(out);
    };
    setAudioSink(forceNull ? AudioSink::createNull(WTF::move(render)) : AudioSink::create(WTF::move(render)));
    m_lastAudioClock.reset();
    ffmpegMediaLog("mse: audio sink: using %s, latency %.0f ms", m_audioSink->name(), m_audioSink->latency() * 1000);
}

void MediaPlayerPrivateFFmpegMSE::setAudioSink(std::unique_ptr<AudioSink>&& sink)
{
    // The clock (read by the presenter thread) lets go of the old sink first.
    m_clock->setAudioSink(nullptr);
    m_audioSink = WTF::move(sink);
    m_clock->setAudioSink(m_audioSink.get());
}

void MediaPlayerPrivateFFmpegMSE::play()
{
    m_paused = false;
    updatePlaybackRunning();
}

void MediaPlayerPrivateFFmpegMSE::pause()
{
    m_paused = true;
    updatePlaybackRunning();
}

void MediaPlayerPrivateFFmpegMSE::updatePlaybackRunning()
{
    double now = monotonicNow();
    bool running = !m_paused && !m_seeking && !m_ended && m_mediaSourcePrivate;
    m_clock->setRunning(running, now);
    if (m_audioSink) {
        if (running && m_rate == 1)
            m_audioSink->play();
        else
            m_audioSink->pause();
    }
    if (RefPtr presenter = m_presenter)
        presenter->wake();
    if (m_mediaSourcePrivate)
        scheduleTick(0_s);
}

void MediaPlayerPrivateFFmpegMSE::setRateDouble(double rate)
{
    if (!(rate > 0) || rate == m_rate)
        return;
    m_rate = rate;
    m_clock->setRate(rate, monotonicNow());
    if (RefPtr decoder = m_audioDecoder)
        decoder->setDiscard(rate != 1);
    updatePlaybackRunning();
}

void MediaPlayerPrivateFFmpegMSE::setVolume(float volume)
{
    m_volume = std::clamp<double>(volume, 0, 1);
    applyGain();
}

void MediaPlayerPrivateFFmpegMSE::setMuted(bool muted)
{
    m_muted = muted;
    applyGain();
}

void MediaPlayerPrivateFFmpegMSE::applyGain()
{
    if (RefPtr decoder = m_audioDecoder)
        decoder->setGain(m_muted ? 0.0f : static_cast<float>(m_volume));
}

void MediaPlayerPrivateFFmpegMSE::setPageIsVisible(bool visible)
{
    if (m_pageVisible == visible)
        return;
    m_pageVisible = visible;
    if (RefPtr decoder = m_videoDecoder)
        decoder->setSuspended(!visible);
}

bool MediaPlayerPrivateFFmpegMSE::hasVideo() const
{
    RefPtr mediaSource = m_mediaSourcePrivate;
    return mediaSource && mediaSource->hasVideo();
}

bool MediaPlayerPrivateFFmpegMSE::hasAudio() const
{
    RefPtr mediaSource = m_mediaSourcePrivate;
    return mediaSource && mediaSource->hasAudio();
}

// MARK: - Time

MediaTime MediaPlayerPrivateFFmpegMSE::duration() const
{
    RefPtr mediaSource = m_mediaSourcePrivate;
    return mediaSource ? mediaSource->duration() : MediaTime::zeroTime();
}

double MediaPlayerPrivateFFmpegMSE::clockTime(double now) const
{
    return m_clock->time(now);
}

double MediaPlayerPrivateFFmpegMSE::playbackPosition() const
{
    if (m_seeking)
        return m_seekTime;
    if (m_ended)
        return m_endTime;
    return std::max(0.0, clockTime(monotonicNow()));
}

MediaTime MediaPlayerPrivateFFmpegMSE::currentTime() const
{
    double time = playbackPosition();
    auto duration = this->duration();
    if (duration.isValid() && duration.isFinite())
        time = std::min(time, duration.toDouble());
    return MediaTime::createWithDouble(std::max(0.0, time));
}

bool MediaPlayerPrivateFFmpegMSE::timeIsProgressing() const
{
    return m_clock->running() && !m_clock->stalled() && !m_seeking && !m_ended;
}

MediaTime MediaPlayerPrivateFFmpegMSE::maxTimeSeekable() const
{
    RefPtr mediaSource = m_mediaSourcePrivate;
    if (!mediaSource)
        return MediaTime::zeroTime();
    auto duration = mediaSource->duration();
    if (duration.isPositiveInfinite()) {
        auto buffered = mediaSource->buffered();
        return buffered.length() ? buffered.maximumBufferedTime() : MediaTime::zeroTime();
    }
    return duration.isValid() ? duration : MediaTime::zeroTime();
}

const PlatformTimeRanges& MediaPlayerPrivateFFmpegMSE::buffered() const
{
    // HTMLMediaElement asks the MediaSource for buffered ranges.
    return PlatformTimeRanges::emptyRanges();
}

Ref<MediaTimePromise> MediaPlayerPrivateFFmpegMSE::seekToTarget(const SeekTarget& target)
{
    if (auto previous = std::exchange(m_seekPromise, std::nullopt))
        previous->reject(PlatformMediaError::Cancelled);
    RefPtr mediaSource = m_mediaSourcePrivate;
    if (!mediaSource)
        return MediaTimePromise::createAndResolve(MediaTime::zeroTime());

    double now = monotonicNow();
    double time = std::max(0.0, target.time.toDouble());
    ffmpegMediaLog("mse: seek to %.3f (from %.3f)", time, currentTime().toDouble());
    unsigned generation = ++m_seekGeneration;
    m_seekStartedAt = now;
    m_seekAnnouncedAt = now;
    for (auto& sourceBuffer : mediaSource->sourceBuffers()) {
        if (sourceBuffer->platformType() == MediaPlatformType::FFmpeg)
            static_cast<SourceBufferPrivateFFmpeg&>(sourceBuffer.get()).noteSeek();
    }
    m_loggedFirstFrameSerial = 0;
    m_seeking = true;
    m_seekReenqueued = false;
    m_seekTime = time;
    m_ended = false;
    if (RefPtr presenter = m_presenter)
        presenter->setSeeking(true, time);
    flushPipeline(m_video, time);
    flushPipeline(m_audio, time);
    m_clock->reset(time, now);
    m_lastSeekWaitLog = now;
    // readyState follows in the tick: notifying the element synchronously
    // would re-enter HTMLMediaElement::seekTask.
    updatePlaybackRunning();

    m_seekPromise.emplace(PlatformMediaError::Cancelled);
    Ref<MediaTimePromise> promise = m_seekPromise->promise();
    mediaSource->waitForTarget(target)->whenSettled(RunLoop::mainSingleton(), [weakThis = WeakPtr { *this }, generation](auto&& result) {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis || generation != protectedThis->m_seekGeneration || !protectedThis->m_seeking)
            return;
        RefPtr mediaSource = protectedThis->m_mediaSourcePrivate;
        if (!result || !mediaSource) {
            ffmpegMediaLog("mse: seek to %.3f abandoned", protectedThis->m_seekTime);
            protectedThis->m_seeking = false;
            if (RefPtr presenter = protectedThis->m_presenter)
                presenter->setSeeking(false, protectedThis->m_seekTime);
            if (auto promise = std::exchange(protectedThis->m_seekPromise, std::nullopt))
                promise->reject(result ? PlatformMediaError::Cancelled : result.error());
            protectedThis->updatePlaybackRunning();
            return;
        }
        MediaTime seekTime = *result;
        double now = monotonicNow();
        ffmpegMediaLog("mse: seek data at %.3f available after %.0f ms", seekTime.toDouble(), (now - protectedThis->m_seekStartedAt) * 1000);
        protectedThis->m_seekTime = seekTime.toDouble();
        if (RefPtr presenter = protectedThis->m_presenter)
            presenter->setSeeking(true, protectedThis->m_seekTime);
        protectedThis->flushPipeline(protectedThis->m_video, protectedThis->m_seekTime);
        protectedThis->flushPipeline(protectedThis->m_audio, protectedThis->m_seekTime);
        protectedThis->m_clock->reset(protectedThis->m_seekTime, now);
        mediaSource->reenqueueMediaForTime(seekTime)->whenSettled(RunLoop::mainSingleton(), [weakThis, generation](auto&&) {
            RefPtr protectedThis = weakThis.get();
            if (!protectedThis || generation != protectedThis->m_seekGeneration || !protectedThis->m_seeking)
                return;
            protectedThis->m_seekReenqueued = true;
            protectedThis->m_seekStartedAt = monotonicNow();
            ffmpegMediaLog("mse: seek samples re-enqueued (queues v %zu a %zu)", protectedThis->m_video.queue ? protectedThis->m_video.queue->level().packets : 0,
                protectedThis->m_audio.queue ? protectedThis->m_audio.queue->level().packets : 0);
            protectedThis->scheduleTick(0_s);
        });
    });
    return promise;
}

void MediaPlayerPrivateFFmpegMSE::finishSeekIfReady(double now)
{
    if (!m_seeking)
        return;
    RefPtr video = m_videoDecoder;
    RefPtr audio = m_audioDecoder;
    FFmpegMedia::MSESeekInputs inputs;
    inputs.dataAvailable = m_seekReenqueued;
    RefPtr frame = currentFrame();
    inputs.videoReady = !video || !m_pageVisible || video->hasFatalError()
        || (frame && frame->serial == m_video.serial) || video->reachedEnd(m_video.serial);
    inputs.audioReady = !audio || audio->hasFatalError() || audio->hasData(m_audio.serial) || audio->drained(m_audio.serial);
    inputs.secondsSinceReenqueue = now - m_seekStartedAt;
    auto phase = FFmpegMedia::mseSeekPhase(inputs);
    if (phase == FFmpegMedia::MSESeekPhase::WaitingForData) {
        // The page has not appended data at the target yet: say what the
        // MediaSource holds, so a stuck seek shows who is waiting for whom.
        if (now - m_lastSeekWaitLog >= statsIntervalSeconds) {
            m_lastSeekWaitLog = now;
            StringBuilder ranges;
            if (RefPtr mediaSource = m_mediaSourcePrivate) {
                auto buffered = mediaSource->buffered();
                for (unsigned i = 0; i < buffered.length() && i < 6; ++i)
                    ranges.append(i ? " "_s : ""_s, FormattedNumber::fixedPrecision(buffered.start(i).toDouble(), 6), '-', FormattedNumber::fixedPrecision(buffered.end(i).toDouble(), 6));
                for (auto& sourceBuffer : mediaSource->sourceBuffers()) {
                    if (sourceBuffer->platformType() == MediaPlatformType::FFmpeg) {
                        auto& buffer = static_cast<SourceBufferPrivateFFmpeg&>(sourceBuffer.get());
                        ranges.append(" | #"_s, buffer.identifier(), ": "_s, buffer.appendsSinceSeek(), " appends since the seek"_s);
                    }
                }
            }
            ffmpegMediaLog("mse: seek to %.3f still waiting for data after %.0f s; buffered %s", m_seekTime, now - m_seekAnnouncedAt,
                ranges.isEmpty() ? "none" : ranges.toString().utf8().legacyCStringPointer());
        }
        return;
    }
    bool timedOut = !inputs.videoReady || !inputs.audioReady;
    bool videoReady = inputs.videoReady;
    bool audioReady = inputs.audioReady;
    if (phase != FFmpegMedia::MSESeekPhase::Done)
        return;

    ffmpegMediaLog("mse: seek finished at %.3f in %.0f ms after re-enqueue%s", m_seekTime, (now - m_seekStartedAt) * 1000,
        timedOut ? makeString(" (timed out waiting for "_s, videoReady ? ""_s : "video "_s, audioReady ? ""_s : "audio"_s, ')').utf8().legacyCStringPointer() : "");
    m_seeking = false;
    m_seekReenqueued = false;
    m_clock->reset(m_seekTime, now);
    if (RefPtr presenter = m_presenter)
        presenter->setSeeking(false, m_seekTime);
    updatePlaybackRunning();
    if (auto promise = std::exchange(m_seekPromise, std::nullopt))
        promise->resolve(MediaTime::createWithDouble(m_seekTime));
    updateReadyState();
    if (RefPtr player = m_player.get())
        player->timeChanged();
}

// MARK: - Tick

void MediaPlayerPrivateFFmpegMSE::scheduleTick(Seconds delay)
{
    if (!m_tickTimer.isActive() || delay < m_tickTimer.secondsUntilFire())
        m_tickTimer.startOneShot(delay);
}

void MediaPlayerPrivateFFmpegMSE::tick()
{
    RefPtr player = m_player.get();
    RefPtr mediaSource = m_mediaSourcePrivate;
    if (!player || !mediaSource)
        return;

    double now = monotonicNow();
    double position = m_seeking ? m_seekTime : clockTime(now);
    // The presenter keeps the video decoder's hint current.
    if (RefPtr decoder = m_audioDecoder)
        decoder->setClockHint(position);

    for (auto* pipeline : { &m_video, &m_audio }) {
        if (!pipeline->wantsMore || !pipeline->queue || !FFmpegMedia::mseQueueShouldRefill(pipeline->queue->level()))
            continue;
        pipeline->wantsMore = false;
        if (RefPtr sourceBuffer = pipeline->sourceBuffer.get())
            sourceBuffer->provideMoreSamples(pipeline->trackID);
    }

    updateOverlay();
    finishSeekIfReady(now);

    auto duration = mediaSource->duration();
    double durationSeconds = duration.isValid() && duration.isFinite() ? duration.toDouble() : std::numeric_limits<double>::infinity();
    auto buffered = toRangeSet(mediaSource->buffered());
    bool stalled = !m_seeking && m_clock->running() && FFmpegMedia::mseShouldStall(buffered, position, durationSeconds, mediaSource->isEnded());
    m_clock->setStalled(stalled, now);
    if (stalled != m_stalled) {
        m_stalled = stalled;
        if (stalled)
            ffmpegMediaLog("mse: stalled at %.3f (%.2f s buffered ahead, %zu ranges)", position, buffered.bufferedAhead(position), buffered.size());
        else
            ffmpegMediaLog("mse: resumed at %.3f (%.2f s buffered ahead)", position, buffered.bufferedAhead(position));
    }

    checkEnded(position);
    updateReadyState();
    checkAudioWatchdog(now);
    logStats(now, position);

    // Frames are the presenter's; this tick keeps the HTML states and the
    // SourceBuffer feeding current.
    if (m_seeking)
        scheduleTick(MediaPlayerPrivateFFmpeg::busyStateTick);
    else if (m_clock->running() && !m_ended)
        scheduleTick(MediaPlayerPrivateFFmpeg::playingStateTick);
    else
        scheduleTick(MediaPlayerPrivateFFmpeg::idleStateTick);
}

void MediaPlayerPrivateFFmpegMSE::overlayTick()
{
    updateOverlay();
    if (m_overlay && m_videoDecoder)
        m_overlayTimer.startOneShot(MediaPlayerPrivateFFmpeg::overlayGeometryTick);
}

void MediaPlayerPrivateFFmpegMSE::startPresenter()
{
    RefPtr decoder = m_videoDecoder;
    if (!decoder || m_presenter)
        return;
    m_presenter = FFmpegVideoPresenter::create(m_clock.copyRef(), decoder.releaseNonNull(), m_video.serial, " [mse]", [this](unsigned events) {
        handlePresenterEvents(events);
    });
    RefPtr presenter = m_presenter;
    presenter->setSeeking(m_seeking, m_seekTime);
    presenter->setOverlay(m_overlay.get());
    presenter->start();
}

void MediaPlayerPrivateFFmpegMSE::stopPresenter()
{
    RefPtr presenter = std::exchange(m_presenter, nullptr);
    if (!presenter)
        return;
    presenter->stop();
    m_retainedFrame = presenter->currentFrame();
    m_presenterDroppedBefore += presenter->droppedFrames();
}

bool MediaPlayerPrivateFFmpegMSE::hasAvailableVideoFrame() const
{
    return !!currentFrame();
}

RefPtr<FFmpegVideoFrame> MediaPlayerPrivateFFmpegMSE::currentFrame() const
{
    if (RefPtr presenter = m_presenter)
        return presenter->currentFrame();
    return m_retainedFrame;
}

// Main thread, coalesced: the presenter thread showed one or more frames.
void MediaPlayerPrivateFFmpegMSE::handlePresenterEvents(unsigned events)
{
    RefPtr player = m_player.get();
    RefPtr frame = currentFrame();
    if (!player || !frame)
        return;
    // Let go of the previous frame's image (it keeps a pool frame alive).
    if (frame != m_imageFrame) {
        m_imageFrame = nullptr;
        m_currentImage = nullptr;
    }
    FloatSize size(frame->width(), frame->height());
    if (size != m_naturalSize) {
        m_naturalSize = size;
        player->sizeChanged();
    }
    if (events & FFmpegVideoPresenter::FirstFrame) {
        if (m_loggedFirstFrameSerial != frame->serial + 1) {
            m_loggedFirstFrameSerial = frame->serial + 1;
            ffmpegMediaLog("mse: first video frame (serial %u): pts %.3f, %dx%d", frame->serial, frame->pts, frame->width(), frame->height());
        }
        player->firstVideoFrameAvailable();
        // Seek completion, readyState and the hole punch follow now.
        scheduleTick(0_s);
        overlayTick();
    }
    if (events & FFmpegVideoPresenter::ReachedEnd)
        scheduleTick(0_s);
    // With the hole punched the overlay shows the frame: no page repaint.
    if ((events & FFmpegVideoPresenter::FrameShown) && (!m_overlay || !m_overlay->punchesHole()))
        player->repaint();
}

void MediaPlayerPrivateFFmpegMSE::updateOverlay()
{
    if (!m_videoDecoder)
        return;
    if (!std::exchange(m_overlayCreated, true)) {
        m_overlay = FFmpegVideoOverlay::createIfEnabled();
        if (RefPtr presenter = m_presenter)
            presenter->setOverlay(m_overlay.get());
        if (m_overlay)
            m_overlayTimer.startOneShot(MediaPlayerPrivateFFmpeg::overlayGeometryTick);
    }
    if (!m_overlay)
        return;
    RefPtr player = m_player.get();
    if (player && m_overlay->update(*player, m_pageVisible, !!currentFrame())) {
        player->repaint();
        if (RefPtr presenter = m_presenter; presenter && m_overlay->punchesHole())
            presenter->resubmitToOverlay();
    }
}

void MediaPlayerPrivateFFmpegMSE::checkEnded(double position)
{
    RefPtr mediaSource = m_mediaSourcePrivate;
    if (m_ended || m_seeking || !mediaSource || !mediaSource->isEnded() || !m_clock->running())
        return;
    auto duration = mediaSource->duration();
    bool pastDuration = duration.isValid() && duration.isFinite() && position >= duration.toDouble();
    RefPtr video = m_videoDecoder;
    RefPtr audio = m_audioDecoder;
    bool videoDone = !video || !m_pageVisible || video->hasFatalError() || (m_video.endOfStream && video->reachedEnd(m_video.serial));
    bool audioDone = !audio || audio->hasFatalError() || (m_audio.endOfStream && audio->drained(m_audio.serial));
    if (video && !m_pageVisible && !audio)
        videoDone = pastDuration;
    if ((!videoDone || !audioDone) && !pastDuration)
        return;

    ffmpegMediaLog("mse: ended at %.3f", position);
    RefPtr player = m_player.get();
    m_ended = true;
    m_endTime = pastDuration || !duration.isFinite() ? position : std::max(position, duration.toDouble());
    if (player && !player->isLooping())
        m_paused = true;
    updatePlaybackRunning();
    if (player)
        player->timeChanged();
}

void MediaPlayerPrivateFFmpegMSE::updateReadyState()
{
    RefPtr mediaSource = m_mediaSourcePrivate;
    if (!mediaSource) {
        setReadyState(MediaPlayer::ReadyState::HaveNothing);
        return;
    }
    RefPtr video = m_videoDecoder;
    RefPtr audio = m_audioDecoder;
    FFmpegMedia::MSEReadyInputs inputs;
    inputs.fromMediaSource = toReadyLevel(mediaSource->mediaPlayerReadyState());
    inputs.seeking = m_seeking;
    inputs.videoRendered = mediaSource->hasVideo() && m_pageVisible && !m_video.failed && !(video && video->hasFatalError());
    RefPtr frame = currentFrame();
    inputs.videoFrameShown = frame && video && frame->serial == m_video.serial;
    inputs.audioReady = !audio || audio->hasFatalError() || audio->hasData(m_audio.serial) || audio->drained(m_audio.serial);
    setReadyState(toReadyState(FFmpegMedia::mseReadyLevel(inputs)));

    bool videoBroken = m_video.failed || (video && video->hasFatalError());
    bool audioBroken = m_audio.failed || (audio && audio->hasFatalError());
    if ((videoBroken || !m_video.bound()) && (audioBroken || !m_audio.bound()) && (videoBroken || audioBroken))
        setNetworkState(MediaPlayer::NetworkState::DecodeError);
}

void MediaPlayerPrivateFFmpegMSE::readyStateFromMediaSourceChanged()
{
    updateReadyState();
    scheduleTick(0_s);
}

void MediaPlayerPrivateFFmpegMSE::characteristicsFromMediaSourceChanged()
{
    if (RefPtr player = m_player.get())
        player->characteristicChanged();
}

void MediaPlayerPrivateFFmpegMSE::seekableRangesFromMediaSourceChanged()
{
    if (RefPtr player = m_player.get())
        player->seekableTimeRangesChanged();
}

void MediaPlayerPrivateFFmpegMSE::mediaSourceHasRetrievedAllData()
{
    setNetworkState(MediaPlayer::NetworkState::Loaded);
    scheduleTick(0_s);
}

void MediaPlayerPrivateFFmpegMSE::checkAudioWatchdog(double now)
{
    RefPtr decoder = m_audioDecoder;
    if (!decoder || !m_audioSink || m_audioSink->isNull() || !m_clock->running() || m_rate != 1 || m_seeking) {
        m_lastAudioClock.reset();
        return;
    }
    auto audioTime = decoder->clockTime(m_audioSink->latency());
    if (!audioTime || decoder->bufferedSeconds() < 0.5) {
        m_lastAudioClock.reset();
        return;
    }
    if (!m_lastAudioClock || *m_lastAudioClock != *audioTime) {
        m_lastAudioClock = audioTime;
        m_lastAudioClockChange = now;
        return;
    }
    if (now - m_lastAudioClockChange < audioWatchdogSeconds)
        return;
    ffmpegMediaLog("mse: audio sink %s stopped pulling samples for %.0f s, switching to the null sink", m_audioSink->name(), audioWatchdogSeconds);
    createAudioSink(true);
    updatePlaybackRunning();
}

void MediaPlayerPrivateFFmpegMSE::logStats(double now, double position)
{
    if (!ffmpegMediaLogEnabled())
        return;
    bool active = (m_clock->running() && !m_ended) || m_seeking;
    if (!active) {
        m_lastStatsAt = 0;
        return;
    }
    if (!m_lastStatsAt) {
        m_lastStatsAt = now;
        return;
    }
    if (now - m_lastStatsAt < statsIntervalSeconds)
        return;
    m_lastStatsAt = now;
    RefPtr video = m_videoDecoder;
    RefPtr audio = m_audioDecoder;
    RefPtr mediaSource = m_mediaSourcePrivate;
    auto videoLevel = m_video.queue ? m_video.queue->level() : FFmpegMedia::QueueLevel { };
    auto audioLevel = m_audio.queue ? m_audio.queue->level() : FFmpegMedia::QueueLevel { };
    std::optional<double> audioClock;
    if (audio && m_audioSink)
        audioClock = audio->clockTime(m_audioSink->latency());
    double ahead = mediaSource ? toRangeSet(mediaSource->buffered()).bufferedAhead(position) : 0;
    auto timing = video ? video->takeTiming() : FFmpegVideoDecoder::Timing { };
    ffmpegMediaLog("mse stats: t=%.2f%s %s | video %u decoded %u dropped (+%u presenter) %zu queued, pts %.2f, decode %.1f ms convert %.1f ms per frame (%d threads) | queues v %zu/%.1fs a %zu/%.1fs | audio %s clock %.2f ring %.2fs | buffered ahead %.1fs, %llu KiB appended",
        position, m_seeking ? " SEEKING" : "", readyStateName(m_readyState),
        video ? video->decodedFrames() : 0, video ? video->droppedFrames() : 0, presenterDroppedFrames(), video ? video->queuedFrames() : 0,
        currentFrame() ? currentFrame()->pts : -1.0, timing.decodeMilliseconds, timing.convertMilliseconds, video ? video->threadCount() : 0,
        videoLevel.packets, videoLevel.seconds, audioLevel.packets, audioLevel.seconds,
        m_audioSink ? m_audioSink->name() : "none", audioClock ? *audioClock : -1.0, audio ? audio->bufferedSeconds() : 0.0,
        ahead, static_cast<unsigned long long>(m_bytesAppended >> 10));
    if (m_overlay)
        m_overlay->logStats(statsIntervalSeconds);
}

// MARK: - States

void MediaPlayerPrivateFFmpegMSE::setNetworkState(MediaPlayer::NetworkState state)
{
    if (m_networkState == state)
        return;
    ffmpegMediaLog("mse: networkState %d -> %d", static_cast<int>(m_networkState), static_cast<int>(state));
    m_networkState = state;
    if (RefPtr player = m_player.get())
        player->networkStateChanged();
}

void MediaPlayerPrivateFFmpegMSE::setReadyState(MediaPlayer::ReadyState state)
{
    if (m_readyState == state)
        return;
    ffmpegMediaLog("mse: readyState %s -> %s at %.3f", readyStateName(m_readyState), readyStateName(state), playbackPosition());
    m_readyState = state;
    if (RefPtr player = m_player.get())
        player->readyStateChanged();
}

bool MediaPlayerPrivateFFmpegMSE::didLoadingProgress() const
{
    bool progressed = m_bytesAppended != m_lastProgressBytes;
    m_lastProgressBytes = m_bytesAppended;
    return progressed;
}

unsigned MediaPlayerPrivateFFmpegMSE::decodedFrameCount() const
{
    RefPtr decoder = m_videoDecoder;
    return decoder ? decoder->decodedFrames() : 0;
}

unsigned MediaPlayerPrivateFFmpegMSE::droppedFrameCount() const
{
    RefPtr decoder = m_videoDecoder;
    return (decoder ? decoder->droppedFrames() : 0) + presenterDroppedFrames();
}

std::optional<VideoPlaybackQualityMetrics> MediaPlayerPrivateFFmpegMSE::videoPlaybackQualityMetrics()
{
    // YouTube lowers the quality when it sees dropped frames.
    VideoPlaybackQualityMetrics metrics;
    metrics.totalVideoFrames = decodedFrameCount();
    metrics.droppedVideoFrames = droppedFrameCount();
    return metrics;
}

// MARK: - Painting

RefPtr<NativeImage> MediaPlayerPrivateFFmpegMSE::currentNativeImage()
{
    RefPtr frame = currentFrame();
    if (frame != m_imageFrame) {
        m_imageFrame = frame;
        m_currentImage = frame ? MediaPlayerPrivateFFmpeg::createNativeImage(*frame) : nullptr;
    }
    return m_currentImage;
}

void MediaPlayerPrivateFFmpegMSE::paint(GraphicsContext& context, const FloatRect& rect)
{
    MediaPlayerPrivateFFmpeg::paintVideo(context, rect, m_overlay.get(), true, m_overlay && m_overlay->punchesHole() ? nullptr : currentNativeImage());
}

void MediaPlayerPrivateFFmpegMSE::paintCurrentFrameInContext(GraphicsContext& context, const FloatRect& rect)
{
    // Canvas drawImage() and snapshots need the pixels, never the hole.
    MediaPlayerPrivateFFmpeg::paintVideo(context, rect, nullptr, false, currentNativeImage());
}

RefPtr<NativeImage> MediaPlayerPrivateFFmpegMSE::nativeImageForCurrentTime()
{
    return currentNativeImage();
}

ColorSpace MediaPlayerPrivateFFmpegMSE::colorSpace()
{
    return ColorSpace::SRGB();
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG) && ENABLE(MEDIA_SOURCE)
