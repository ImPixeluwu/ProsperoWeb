// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "MediaPlayerPrivateFFmpeg.h"

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "AudioSink.h"
#include "FFmpegAudioDecoder.h"
#include "FFmpegDemuxer.h"
#include "FFmpegSupportTable.h"
#include "FFmpegVideoDecoder.h"
#include "GraphicsContext.h"
#include "Logging.h"
#include "MediaPlayer.h"
#include "NativeImage.h"
#include "PlatformMediaResourceLoader.h"
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkPixmap.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(MediaPlayerPrivateFFmpeg);

namespace {

// Presentation tick: fast enough to hit 60 fps frame boundaries while
// playing video, slow otherwise.
constexpr Seconds playingVideoTick = 8_ms;
constexpr Seconds playingAudioTick = 50_ms;
constexpr Seconds idleTick = 100_ms;
// Below this much demuxed data ahead the extrapolated clock stalls
// (buffering) unless the stream has ended.
constexpr double stallThreshold = 0.1;
constexpr double audioWatchdogSeconds = 2.0;

double monotonicNow()
{
    return MonotonicTime::now().secondsSinceEpoch().seconds();
}

const FFmpegMedia::DecoderAvailability& decoderAvailability()
{
    static NeverDestroyed<FFmpegMedia::DecoderAvailability> decoders = [] {
        FFmpegMedia::DecoderAvailability available;
        available.h264 = findSoftwareDecoder(AV_CODEC_ID_H264);
        available.hevc = findSoftwareDecoder(AV_CODEC_ID_HEVC);
        available.vp8 = findSoftwareDecoder(AV_CODEC_ID_VP8);
        available.vp9 = findSoftwareDecoder(AV_CODEC_ID_VP9);
        available.av1 = findSoftwareDecoder(AV_CODEC_ID_AV1);
        available.aac = findSoftwareDecoder(AV_CODEC_ID_AAC);
        available.mp3 = findSoftwareDecoder(AV_CODEC_ID_MP3);
        available.opus = findSoftwareDecoder(AV_CODEC_ID_OPUS);
        available.vorbis = findSoftwareDecoder(AV_CODEC_ID_VORBIS);
        available.flac = findSoftwareDecoder(AV_CODEC_ID_FLAC);
        available.ac3 = findSoftwareDecoder(AV_CODEC_ID_AC3);
        available.eac3 = findSoftwareDecoder(AV_CODEC_ID_EAC3);
        return available;
    }();
    return decoders.get();
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

MediaPlayer::NetworkState toNetworkState(FFmpegMedia::NetworkLevel level)
{
    switch (level) {
    case FFmpegMedia::NetworkLevel::Empty: return MediaPlayer::NetworkState::Empty;
    case FFmpegMedia::NetworkLevel::Idle: return MediaPlayer::NetworkState::Idle;
    case FFmpegMedia::NetworkLevel::Loading: return MediaPlayer::NetworkState::Loading;
    case FFmpegMedia::NetworkLevel::Loaded: return MediaPlayer::NetworkState::Loaded;
    case FFmpegMedia::NetworkLevel::FormatError: return MediaPlayer::NetworkState::FormatError;
    case FFmpegMedia::NetworkLevel::NetworkError: return MediaPlayer::NetworkState::NetworkError;
    case FFmpegMedia::NetworkLevel::DecodeError: return MediaPlayer::NetworkState::DecodeError;
    }
    return MediaPlayer::NetworkState::Empty;
}

} // namespace

// MARK: - Registration

class MediaPlayerFactoryFFmpeg final : public MediaPlayerFactory {
    WTF_MAKE_TZONE_ALLOCATED(MediaPlayerFactoryFFmpeg);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(MediaPlayerFactoryFFmpeg);
private:
    MediaPlayerEnums::MediaEngineIdentifier identifier() const final { return MediaPlayerEnums::MediaEngineIdentifier::FFmpeg; }

    Ref<MediaPlayerPrivateInterface> createMediaEnginePlayer(MediaPlayer& player) const final
    {
        return adoptRef(*new MediaPlayerPrivateFFmpeg(player));
    }

    void getSupportedTypes(HashSet<String>& types) const final
    {
        MediaPlayerPrivateFFmpeg::getSupportedTypes(types);
    }

    MediaPlayer::SupportsType supportsTypeAndCodecs(const MediaEngineSupportParameters& parameters) const final
    {
        return MediaPlayerPrivateFFmpeg::supportsType(parameters);
    }
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(MediaPlayerFactoryFFmpeg);

void MediaPlayerPrivateFFmpeg::registerMediaEngine(MediaEngineRegistrar registrar)
{
    registrar(makeUnique<MediaPlayerFactoryFFmpeg>());
}

void MediaPlayerPrivateFFmpeg::getSupportedTypes(HashSet<String>& types)
{
    for (auto& entry : FFmpegMedia::containerTable)
        types.add(String::fromLatin1(std::string(entry.mimeType).c_str()));
}

MediaPlayer::SupportsType MediaPlayerPrivateFFmpeg::supportsType(const MediaEngineSupportParameters& parameters)
{
    // MSE and MediaStream are not implemented by this engine.
    if (parameters.platformType != PlatformMediaDecodingType::FileOrHLS)
        return MediaPlayer::SupportsType::IsNotSupported;

    auto container = parameters.type.containerType();
    if (container.isEmpty())
        return MediaPlayer::SupportsType::IsNotSupported;

    std::vector<std::string> codecs;
    for (auto& codec : parameters.type.codecs())
        codecs.push_back(codec.utf8().toStdString());
    auto containerString = container.utf8().toStdString();
    switch (FFmpegMedia::evaluate(containerString, codecs, decoderAvailability())) {
    case FFmpegMedia::Support::IsSupported:
        return MediaPlayer::SupportsType::IsSupported;
    case FFmpegMedia::Support::MaybeSupported:
        return MediaPlayer::SupportsType::MayBeSupported;
    case FFmpegMedia::Support::NotSupported:
        break;
    }
    return MediaPlayer::SupportsType::IsNotSupported;
}

// MARK: - Lifetime

MediaPlayerPrivateFFmpeg::MediaPlayerPrivateFFmpeg(MediaPlayer& player)
    : m_player(player)
    , m_tickTimer(RunLoop::mainSingleton(), "MediaPlayerPrivateFFmpeg::Tick"_s, this, &MediaPlayerPrivateFFmpeg::tick)
{
}

MediaPlayerPrivateFFmpeg::~MediaPlayerPrivateFFmpeg()
{
    stopPipeline();
}

void MediaPlayerPrivateFFmpeg::stopPipeline()
{
    m_tickTimer.stop();
    // Order matters: the sink calls into the audio decoder, the demuxer owns
    // the packet queues the decoders read (stopping it aborts them).
    m_audioSink = nullptr;
    if (RefPtr demuxer = m_demuxer)
        demuxer->stop();
    if (RefPtr decoder = m_videoDecoder)
        decoder->stop();
    if (RefPtr decoder = m_audioDecoder)
        decoder->stop();
    m_videoDecoder = nullptr;
    m_audioDecoder = nullptr;
    m_demuxer = nullptr;
    if (auto promise = std::exchange(m_seekPromise, std::nullopt))
        promise->reject(PlatformMediaError::Cancelled);
    m_seeking = false;
}

// MARK: - Loading

void MediaPlayerPrivateFFmpeg::load(const String& urlString)
{
    RefPtr player = m_player.get();
    if (!player)
        return;

    stopPipeline();
    m_loadStarted = false;
    m_url = URL { urlString };
    m_metadataLoaded = false;
    m_failed = false;
    m_ended = false;
    m_hasVideo = m_hasAudio = false;
    m_currentFrame = nullptr;
    m_currentImage = nullptr;
    m_buffered = { };

    if (!m_url.isValid()) {
        setNetworkState(MediaPlayer::NetworkState::FormatError);
        return;
    }

    setNetworkState(MediaPlayer::NetworkState::Loading);
    setReadyState(MediaPlayer::ReadyState::HaveNothing);

    if (m_preload == MediaPlayer::Preload::None) {
        // Wait for prepareToPlay()/play() (HTML preload="none").
        m_loadDeferred = true;
        setNetworkState(MediaPlayer::NetworkState::Idle);
        return;
    }
    startLoading();
}

#if ENABLE(MEDIA_SOURCE)
void MediaPlayerPrivateFFmpeg::load(const URL&, const LoadOptions&, MediaSourcePrivateClient&)
{
    // MSE is not supported yet; supportsType() never selects this engine for it.
    setNetworkState(MediaPlayer::NetworkState::FormatError);
}
#endif

#if ENABLE(MEDIA_STREAM)
void MediaPlayerPrivateFFmpeg::load(MediaStreamPrivate&)
{
    setNetworkState(MediaPlayer::NetworkState::FormatError);
}
#endif

void MediaPlayerPrivateFFmpeg::startLoading()
{
    RefPtr player = m_player.get();
    if (!player || m_loadStarted)
        return;
    m_loadDeferred = false;
    m_loadStarted = true;
    m_demuxer = FFmpegDemuxer::create(m_url, player->mediaResourceLoader(), player->userAgent());
    m_demuxer->start();
    setNetworkState(MediaPlayer::NetworkState::Loading);
    scheduleTick(0_s);
}

void MediaPlayerPrivateFFmpeg::cancelLoad()
{
    stopPipeline();
    m_loadStarted = false;
    m_loadDeferred = false;
    setNetworkState(MediaPlayer::NetworkState::Idle);
}

void MediaPlayerPrivateFFmpeg::prepareToPlay()
{
    if (m_loadDeferred)
        startLoading();
}

void MediaPlayerPrivateFFmpeg::setPreload(MediaPlayer::Preload preload)
{
    m_preload = preload;
    if (preload != MediaPlayer::Preload::None && m_loadDeferred)
        startLoading();
}

void MediaPlayerPrivateFFmpeg::onMetadataLoaded()
{
    RefPtr player = m_player.get();
    RefPtr demuxer = m_demuxer;
    if (!player || !demuxer)
        return;

    if (auto* stream = demuxer->videoStream())
        m_videoDecoder = FFmpegVideoDecoder::create(*stream, demuxer->videoQueue());
    if (auto* stream = demuxer->audioStream())
        m_audioDecoder = FFmpegAudioDecoder::create(*stream, demuxer->audioQueue(), AudioSink::sampleRate, AudioSink::channels);
    if (!m_videoDecoder && !m_audioDecoder) {
        m_failed = true;
        setNetworkState(MediaPlayer::NetworkState::FormatError);
        return;
    }

    m_metadataLoaded = true;
    m_hasVideo = !!m_videoDecoder;
    m_hasAudio = !!m_audioDecoder;
    m_isLive = demuxer->isLive();
    m_isSeekable = demuxer->isSeekable();
    m_duration = demuxer->duration();

    if (RefPtr decoder = m_videoDecoder) {
        m_naturalSize = FloatSize(decoder->outputWidth(), decoder->outputHeight());
        decoder->setSuspended(!m_pageVisible);
        decoder->start();
    }
    if (RefPtr decoder = m_audioDecoder) {
        decoder->start();
        createAudioSink(false);
        applyGain();
        decoder->setDiscard(m_rate != 1);
    }
    m_clock.reset(0, monotonicNow());
    m_clock.setRate(m_rate, monotonicNow());

    player->durationChanged();
    player->sizeChanged();
    player->characteristicChanged();
    setReadyState(MediaPlayer::ReadyState::HaveMetadata);
    updatePlaybackRunning();
}

void MediaPlayerPrivateFFmpeg::createAudioSink(bool forceNull)
{
    RefPtr decoder = m_audioDecoder;
    if (!decoder)
        return;
    m_audioSink = nullptr;
    auto render = [decoder](std::span<float> out) {
        decoder->render(out);
    };
    m_audioSink = forceNull ? AudioSink::createNull(WTF::move(render)) : AudioSink::create(WTF::move(render));
    m_lastAudioClock.reset();
    LOG(Media, "MediaPlayerPrivateFFmpeg: audio sink %s", m_audioSink->name());
}

// MARK: - Playback control

void MediaPlayerPrivateFFmpeg::play()
{
    m_paused = false;
    if (m_loadDeferred)
        startLoading();
    updatePlaybackRunning();
}

void MediaPlayerPrivateFFmpeg::pause()
{
    m_paused = true;
    updatePlaybackRunning();
}

void MediaPlayerPrivateFFmpeg::updatePlaybackRunning()
{
    double now = monotonicNow();
    bool running = !m_paused && m_metadataLoaded && !m_seeking && !m_ended;
    m_clock.setRunning(running, now);
    if (m_audioSink) {
        if (running && m_rate == 1)
            m_audioSink->play();
        else
            m_audioSink->pause();
    }
    if (m_demuxer)
        scheduleTick(0_s);
}

void MediaPlayerPrivateFFmpeg::setRateDouble(double rate)
{
    if (!(rate > 0) || rate == m_rate)
        return;
    m_rate = rate;
    m_clock.setRate(rate, monotonicNow());
    if (RefPtr decoder = m_audioDecoder)
        decoder->setDiscard(rate != 1);
    updatePlaybackRunning();
}

void MediaPlayerPrivateFFmpeg::setVolume(float volume)
{
    m_volume = std::clamp<double>(volume, 0, 1);
    applyGain();
}

void MediaPlayerPrivateFFmpeg::setMuted(bool muted)
{
    m_muted = muted;
    applyGain();
}

void MediaPlayerPrivateFFmpeg::applyGain()
{
    if (RefPtr decoder = m_audioDecoder)
        decoder->setGain(m_muted ? 0.0f : static_cast<float>(m_volume));
}

void MediaPlayerPrivateFFmpeg::setPageIsVisible(bool visible)
{
    if (m_pageVisible == visible)
        return;
    m_pageVisible = visible;
    // Hidden: stop decoding and converting video (audio keeps playing);
    // decoding resumes at the next keyframe.
    if (RefPtr decoder = m_videoDecoder)
        decoder->setSuspended(!visible);
}

// MARK: - Time

MediaTime MediaPlayerPrivateFFmpeg::duration() const
{
    if (!m_metadataLoaded)
        return MediaTime::zeroTime();
    if (m_isLive || !std::isfinite(m_duration))
        return MediaTime::positiveInfiniteTime();
    return MediaTime::createWithDouble(m_duration);
}

double MediaPlayerPrivateFFmpeg::clockTime(double now) const
{
    std::optional<double> audioTime;
    if (RefPtr decoder = m_audioDecoder; decoder && m_audioSink && !decoder->drained(m_serial))
        audioTime = decoder->clockTime(m_audioSink->latency());
    return m_clock.time(now, audioTime);
}

MediaTime MediaPlayerPrivateFFmpeg::currentTime() const
{
    if (!m_metadataLoaded)
        return MediaTime::zeroTime();
    double time;
    if (m_seeking)
        time = m_seekTime;
    else if (m_ended)
        time = m_endTime;
    else
        time = clockTime(monotonicNow());
    if (std::isfinite(m_duration) && !m_isLive)
        time = std::min(time, m_duration);
    return MediaTime::createWithDouble(std::max(0.0, time));
}

bool MediaPlayerPrivateFFmpeg::timeIsProgressing() const
{
    return m_clock.running() && !m_clock.stalled() && !m_seeking && !m_ended;
}

MediaTime MediaPlayerPrivateFFmpeg::maxTimeSeekable() const
{
    if (!m_metadataLoaded || !m_isSeekable || !std::isfinite(m_duration))
        return MediaTime::zeroTime();
    return MediaTime::createWithDouble(m_duration);
}

Ref<MediaTimePromise> MediaPlayerPrivateFFmpeg::seekToTarget(const SeekTarget& target)
{
    if (auto previous = std::exchange(m_seekPromise, std::nullopt))
        previous->reject(PlatformMediaError::Cancelled);

    RefPtr demuxer = m_demuxer;
    if (!m_metadataLoaded || !demuxer)
        return MediaTimePromise::createAndResolve(MediaTime::zeroTime());
    if (!m_isSeekable)
        return MediaTimePromise::createAndResolve(currentTime());

    double time = std::max(0.0, target.time.toDouble());
    if (std::isfinite(m_duration))
        time = std::min(time, m_duration);

    ++m_serial;
    m_seekTime = time;
    m_seeking = true;
    m_ended = false;
    // Decoders first, so the first packets of the new serial are trimmed.
    if (RefPtr decoder = m_videoDecoder)
        decoder->setSeekTarget(time, m_serial);
    if (RefPtr decoder = m_audioDecoder)
        decoder->setSeekTarget(time, m_serial);
    demuxer->seek(time, m_serial);
    m_clock.reset(time, monotonicNow());
    updatePlaybackRunning();

    m_seekPromise.emplace(PlatformMediaError::Cancelled);
    return m_seekPromise->promise();
}

void MediaPlayerPrivateFFmpeg::finishSeekIfReady(double now)
{
    if (!m_seeking)
        return;
    RefPtr video = m_videoDecoder;
    RefPtr audio = m_audioDecoder;
    bool videoReady = !video || !m_pageVisible || video->hasFatalError()
        || (m_currentFrame && m_currentFrame->serial == m_serial) || video->reachedEnd(m_serial);
    bool audioReady = !audio || audio->hasFatalError() || audio->hasData(m_serial) || audio->drained(m_serial);
    if (!videoReady || !audioReady)
        return;

    m_seeking = false;
    m_clock.reset(m_seekTime, now);
    updatePlaybackRunning();
    if (auto promise = std::exchange(m_seekPromise, std::nullopt))
        promise->resolve(MediaTime::createWithDouble(m_seekTime));
    if (RefPtr player = m_player.get())
        player->timeChanged();
}

// MARK: - Tick

void MediaPlayerPrivateFFmpeg::scheduleTick(Seconds delay)
{
    if (!m_tickTimer.isActive() || delay < m_tickTimer.secondsUntilFire())
        m_tickTimer.startOneShot(delay);
}

void MediaPlayerPrivateFFmpeg::tick()
{
    RefPtr player = m_player.get();
    RefPtr demuxer = m_demuxer;
    if (!player || !demuxer || m_failed)
        return;

    auto status = demuxer->status();
    double now = monotonicNow();

    if (!m_metadataLoaded) {
        if (status.state == FFmpegDemuxer::State::Failed) {
            updateStates(false, false, 0, 0, status.failure);
            return;
        }
        if (status.state != FFmpegDemuxer::State::Ready) {
            scheduleTick(50_ms);
            return;
        }
        onMetadataLoaded();
        if (!m_metadataLoaded)
            return;
    }

    double position = clockTime(now);
    if (RefPtr decoder = m_videoDecoder)
        decoder->setClockHint(position);
    if (RefPtr decoder = m_audioDecoder)
        decoder->setClockHint(position);

    presentVideo(m_seeking ? m_seekTime : position);
    finishSeekIfReady(now);

    double bufferedAhead = status.buffered.bufferedAhead(m_seeking ? m_seekTime : position);
    // Extrapolated clock (no audio master): hold still while starved.
    m_clock.setStalled(!status.endOfStream && bufferedAhead < stallThreshold && !m_seeking, now);

    checkEnded(status.endOfStream, position);
    updateBuffered(status.buffered);
    updateStates(status.endOfStream, status.readerWaiting, position, bufferedAhead, status.failure);
    checkAudioWatchdog(now);

    if (m_failed)
        return;
    bool playing = m_clock.running() && !m_ended;
    if (playing && m_hasVideo && m_pageVisible)
        scheduleTick(playingVideoTick);
    else if (playing || m_seeking)
        scheduleTick(playingAudioTick);
    else
        scheduleTick(idleTick);
}

void MediaPlayerPrivateFFmpeg::presentVideo(double position)
{
    RefPtr decoder = m_videoDecoder;
    if (!decoder)
        return;

    bool needFirstFrame = !m_currentFrame || m_currentFrame->serial != m_serial;
    RefPtr<FFmpegVideoFrame> chosen;
    while (RefPtr frame = decoder->peekFrame(m_serial)) {
        RefPtr next = decoder->peekFrame(m_serial, 1);
        std::optional<double> nextPts;
        if (next)
            nextPts = next->pts;
        auto decision = FFmpegMedia::decideFrame(frame->pts, nextPts, position);
        // Paused, or just seeked: show the first frame at the new position
        // even though the clock has not reached it.
        if (decision == FFmpegMedia::FrameDecision::Wait && needFirstFrame && !chosen && (m_seeking || !m_clock.running()))
            decision = FFmpegMedia::FrameDecision::Show;
        if (decision == FFmpegMedia::FrameDecision::Wait)
            break;
        decoder->popFrame();
        if (decision == FFmpegMedia::FrameDecision::Drop) {
            ++m_presenterDropped;
            continue;
        }
        chosen = WTF::move(frame);
        if (needFirstFrame && !m_clock.running())
            break;
    }
    if (!chosen)
        return;

    m_currentFrame = WTF::move(chosen);
    m_currentImage = nullptr;
    FloatSize size(m_currentFrame->width(), m_currentFrame->height());
    RefPtr player = m_player.get();
    if (size != m_naturalSize) {
        m_naturalSize = size;
        if (player)
            player->sizeChanged();
    }
    if (player) {
        if (needFirstFrame)
            player->firstVideoFrameAvailable();
        player->repaint();
    }
}

void MediaPlayerPrivateFFmpeg::checkEnded(bool endOfStream, double position)
{
    if (m_ended || m_seeking || !endOfStream || !m_clock.running())
        return;
    RefPtr video = m_videoDecoder;
    RefPtr audio = m_audioDecoder;
    bool videoDone = !video || !m_pageVisible || video->hasFatalError() || video->reachedEnd(m_serial);
    bool audioDone = !audio || audio->hasFatalError() || audio->drained(m_serial);
    // Hidden video: rely on the audio end, or on the clock reaching the end.
    if (video && !m_pageVisible && !audio)
        videoDone = std::isfinite(m_duration) && position >= m_duration;
    if (!videoDone || !audioDone)
        return;

    RefPtr player = m_player.get();
    m_ended = true;
    m_endTime = std::isfinite(m_duration) ? std::max(position, m_duration) : position;
    if (player && !player->isLooping())
        m_paused = true;
    updatePlaybackRunning();
    if (player)
        player->timeChanged();
}

void MediaPlayerPrivateFFmpeg::updateBuffered(const FFmpegMedia::TimeRangeSet& ranges)
{
    PlatformTimeRanges buffered;
    for (auto& range : ranges.ranges()) {
        double start = std::max(0.0, range.start);
        double end = range.end;
        if (std::isfinite(m_duration))
            end = std::min(end, m_duration);
        if (end > start)
            buffered.add(MediaTime::createWithDouble(start), MediaTime::createWithDouble(end));
    }
    m_buffered = WTF::move(buffered);
}

void MediaPlayerPrivateFFmpeg::updateStates(bool endOfStream, bool readerWaiting, double position, double bufferedAhead, FFmpegMedia::LoadFailure failure)
{
    RefPtr video = m_videoDecoder;
    RefPtr audio = m_audioDecoder;
    if (failure == FFmpegMedia::LoadFailure::None && m_metadataLoaded) {
        bool videoBroken = !video || video->hasFatalError();
        bool audioBroken = !audio || audio->hasFatalError();
        if (videoBroken && audioBroken)
            failure = FFmpegMedia::LoadFailure::Decode;
    }

    FFmpegMedia::NetworkInputs network;
    network.started = m_loadStarted;
    network.metadataLoaded = m_metadataLoaded;
    network.failure = failure;
    network.endOfStream = endOfStream;
    network.readerWaiting = readerWaiting;
    auto networkLevel = FFmpegMedia::computeNetworkLevel(network);
    if (networkLevel == FFmpegMedia::NetworkLevel::FormatError || networkLevel == FFmpegMedia::NetworkLevel::NetworkError
        || networkLevel == FFmpegMedia::NetworkLevel::DecodeError) {
        m_failed = true;
        m_paused = true;
        updatePlaybackRunning();
    }
    setNetworkState(toNetworkState(networkLevel));
    if (m_failed || !m_metadataLoaded)
        return;

    FFmpegMedia::ReadyInputs ready;
    ready.metadataLoaded = true;
    ready.seeking = m_seeking;
    if (video && m_pageVisible)
        ready.currentDataAvailable = m_currentFrame && m_currentFrame->serial == m_serial;
    else if (audio)
        ready.currentDataAvailable = audio->hasData(m_serial) || audio->drained(m_serial);
    else
        ready.currentDataAvailable = bufferedAhead > 0 || endOfStream;
    ready.bufferedAheadSeconds = bufferedAhead;
    ready.endOfStream = endOfStream || m_ended;
    UNUSED_PARAM(position);
    setReadyState(toReadyState(FFmpegMedia::computeReadyLevel(ready)));
}

void MediaPlayerPrivateFFmpeg::checkAudioWatchdog(double now)
{
    RefPtr decoder = m_audioDecoder;
    if (!decoder || !m_audioSink || m_audioSink->isNull() || !m_clock.running() || m_rate != 1 || m_seeking) {
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
    WTFLogAlways("[ffmpeg-media] audio output %s stopped pulling samples, switching to the silent sink", m_audioSink->name());
    createAudioSink(true);
    updatePlaybackRunning();
}

// MARK: - States

void MediaPlayerPrivateFFmpeg::setNetworkState(MediaPlayer::NetworkState state)
{
    if (m_networkState == state)
        return;
    m_networkState = state;
    if (RefPtr player = m_player.get())
        player->networkStateChanged();
}

void MediaPlayerPrivateFFmpeg::setReadyState(MediaPlayer::ReadyState state)
{
    if (m_readyState == state)
        return;
    m_readyState = state;
    if (RefPtr player = m_player.get())
        player->readyStateChanged();
}

bool MediaPlayerPrivateFFmpeg::didLoadingProgress() const
{
    RefPtr demuxer = m_demuxer;
    if (!demuxer)
        return false;
    uint64_t bytes = demuxer->status().bytesReceived;
    bool progressed = bytes != m_lastProgressBytes;
    m_lastProgressBytes = bytes;
    return progressed;
}

MediaPlayer::MovieLoadType MediaPlayerPrivateFFmpeg::movieLoadType() const
{
    if (!m_metadataLoaded)
        return MediaPlayer::MovieLoadType::Unknown;
    if (m_isLive)
        return MediaPlayer::MovieLoadType::LiveStream;
    return m_demuxer && m_demuxer->isHLS() ? MediaPlayer::MovieLoadType::StoredStream : MediaPlayer::MovieLoadType::Download;
}

bool MediaPlayerPrivateFFmpeg::didPassCORSAccessCheck() const
{
    return m_demuxer && m_demuxer->didPassAccessControlCheck();
}

unsigned MediaPlayerPrivateFFmpeg::decodedFrameCount() const
{
    RefPtr decoder = m_videoDecoder;
    return decoder ? decoder->decodedFrames() : 0;
}

unsigned MediaPlayerPrivateFFmpeg::droppedFrameCount() const
{
    RefPtr decoder = m_videoDecoder;
    return (decoder ? decoder->droppedFrames() : 0) + m_presenterDropped;
}

// MARK: - Painting

RefPtr<NativeImage> MediaPlayerPrivateFFmpeg::currentNativeImage()
{
    if (m_currentImage || !m_currentFrame)
        return m_currentImage;

    // The SkImage wraps the frame's pixels without copying and keeps the
    // frame alive; the decoder's pool only reuses frames nobody references.
    Ref frame = *m_currentFrame;
    auto info = SkImageInfo::Make(frame->width(), frame->height(), kBGRA_8888_SkColorType, kOpaque_SkAlphaType, SkColorSpace::MakeSRGB());
    SkPixmap pixmap(info, frame->data(), frame->stride());
    auto image = SkImages::RasterFromPixmap(pixmap, [](const void*, void* context) {
        static_cast<FFmpegVideoFrame*>(context)->deref();
    }, frame.ptr());
    if (!image)
        return nullptr;
    // Owned by the SkImage from now on (released by the callback above).
    (void)frame.leakRef();
    m_currentImage = NativeImage::create(WTF::move(image));
    return m_currentImage;
}

void MediaPlayerPrivateFFmpeg::paint(GraphicsContext& context, const FloatRect& rect)
{
    if (context.paintingDisabled() || rect.isEmpty())
        return;
    RefPtr image = currentNativeImage();
    if (!image)
        return;
    context.drawNativeImage(*image, rect, FloatRect { { }, image->size() });
}

RefPtr<NativeImage> MediaPlayerPrivateFFmpeg::nativeImageForCurrentTime()
{
    return currentNativeImage();
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
