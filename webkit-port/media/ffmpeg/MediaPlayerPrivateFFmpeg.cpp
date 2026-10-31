// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "MediaPlayerPrivateFFmpeg.h"

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "AudioSink.h"
#include "FFmpegAudioDecoder.h"
#include "FFmpegDemuxer.h"
#include "FFmpegLog.h"
#include "FFmpegSupportTable.h"
#include "FFmpegVideoDecoder.h"
#include "FFmpegVideoOverlay.h"
#include "GraphicsContext.h"
#include "Logging.h"
#include "MediaPlayer.h"
#include "NativeImage.h"
#include "PlatformMediaResourceLoader.h"
#include <skia/core/SkColorSpace.h>
#include <skia/core/SkImage.h>
#include <skia/core/SkPixmap.h>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(MediaPlayerPrivateFFmpeg);

namespace {

// Below this much demuxed data ahead the extrapolated clock stalls
// (buffering) unless the stream has ended.
constexpr double stallThreshold = 0.1;
constexpr double audioWatchdogSeconds = 2.0;
// Console log: pipeline statistics while playing or seeking.
constexpr double statsIntervalSeconds = 5.0;

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

const char* networkStateName(MediaPlayer::NetworkState state)
{
    switch (state) {
    case MediaPlayer::NetworkState::Empty: return "Empty";
    case MediaPlayer::NetworkState::Idle: return "Idle";
    case MediaPlayer::NetworkState::Loading: return "Loading";
    case MediaPlayer::NetworkState::Loaded: return "Loaded";
    case MediaPlayer::NetworkState::FormatError: return "FormatError";
    case MediaPlayer::NetworkState::NetworkError: return "NetworkError";
    case MediaPlayer::NetworkState::DecodeError: return "DecodeError";
    }
    return "?";
}

const char* supportName(MediaPlayer::SupportsType type)
{
    switch (type) {
    case MediaPlayer::SupportsType::IsNotSupported: return "\"\"";
    case MediaPlayer::SupportsType::IsSupported: return "probably";
    case MediaPlayer::SupportsType::MayBeSupported: return "maybe";
    }
    return "?";
}

// av_malloc() relies on the SDK libc's posix_memalign() for libavcodec's
// 64-byte SIMD alignment; check it once per process.
void checkAllocatorAlignment()
{
    static std::once_flag once;
    std::call_once(once, [] {
        bool aligned = true;
        for (size_t size : { 24u, 1000u, 65536u, 1u << 20 }) {
            void* block = av_malloc(size);
            aligned = aligned && block && !(reinterpret_cast<uintptr_t>(block) & 63);
            av_free(block);
        }
        ffmpegMediaLog("av_malloc 64-byte alignment: %s", aligned ? "ok" : "BROKEN");
    });
}

double monotonicNow()
{
    return MonotonicTime::now().secondsSinceEpoch().seconds();
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

// Logs each distinct canPlayType/source-selection query once (sites ask the
// same questions over and over).
void MediaPlayerPrivateFFmpeg::logSupportsType(const MediaEngineSupportParameters& parameters, MediaPlayer::SupportsType result)
{
    if (!ffmpegMediaLogEnabled())
        return;
    static Lock lock;
    static NeverDestroyed<HashSet<String>> seen;
    String key = makeString(parameters.type.raw(), parameters.platformType == PlatformMediaDecodingType::FileOrHLS ? ""_s : " (MSE/stream)"_s);
    {
        Locker locker { lock };
        if (seen->size() >= 64 || !seen->add(key).isNewEntry)
            return;
    }
    ffmpegMediaLog("supportsType %s -> %s", key.utf8().legacyCStringPointer(), supportName(result));
}

const FFmpegMedia::DecoderAvailability& MediaPlayerPrivateFFmpeg::decoderAvailability()
{
    static NeverDestroyed<FFmpegMedia::DecoderAvailability> decoders = [] {
        FFmpegMedia::DecoderAvailability available;
        available.h264 = findSoftwareDecoder(AV_CODEC_ID_H264);
        available.hevc = findSoftwareDecoder(AV_CODEC_ID_HEVC);
        // Software VP9 costs far more CPU than H.264 at the same size;
        // answering "" makes YouTube & co. serve H.264 instead.
        const char* vp9 = getenv("PROSPEROWEB_MEDIA_VP9");
        bool allowVP9 = vp9 && !strcmp(vp9, "1");
        available.vp8 = allowVP9 && findSoftwareDecoder(AV_CODEC_ID_VP8);
        available.vp9 = allowVP9 && findSoftwareDecoder(AV_CODEC_ID_VP9);
        available.av1 = findSoftwareDecoder(AV_CODEC_ID_AV1);
        available.aac = findSoftwareDecoder(AV_CODEC_ID_AAC);
        available.mp3 = findSoftwareDecoder(AV_CODEC_ID_MP3);
        available.opus = findSoftwareDecoder(AV_CODEC_ID_OPUS);
        available.vorbis = findSoftwareDecoder(AV_CODEC_ID_VORBIS);
        available.flac = findSoftwareDecoder(AV_CODEC_ID_FLAC);
        available.ac3 = findSoftwareDecoder(AV_CODEC_ID_AC3);
        available.eac3 = findSoftwareDecoder(AV_CODEC_ID_EAC3);
        ffmpegMediaLog("codecs: VP8/VP9 %s (PROSPEROWEB_MEDIA_VP9), AV1 %s", available.vp9 ? "on" : "off", available.av1 ? "on" : "off");
        return available;
    }();
    return decoders.get();
}

const FFmpegMedia::VideoLimits& MediaPlayerPrivateFFmpeg::videoLimits()
{
    static NeverDestroyed<FFmpegMedia::VideoLimits> limits = [] {
        FFmpegMedia::VideoLimits value;
        if (const char* text = getenv("PROSPEROWEB_MEDIA_MAX_VIDEO"); text && !FFmpegMedia::parseVideoLimits(text, value))
            value = { };
        ffmpegMediaLog("codecs: video limit %.0fx%.0f (PROSPEROWEB_MEDIA_MAX_VIDEO)", value.maxLongSide, value.maxShortSide);
        return value;
    }();
    return limits.get();
}

void MediaPlayerPrivateFFmpeg::getSupportedTypes(HashSet<String>& types)
{
    for (auto& entry : FFmpegMedia::containerTable)
        types.add(String::fromLatin1(std::string(entry.mimeType).c_str()));
}

MediaPlayer::SupportsType MediaPlayerPrivateFFmpeg::supportsType(const MediaEngineSupportParameters& parameters)
{
    // MediaSource is MediaPlayerPrivateFFmpegMSE's (which logs those
    // queries), MediaStream is not implemented.
    if (parameters.platformType != PlatformMediaDecodingType::FileOrHLS)
        return MediaPlayer::SupportsType::IsNotSupported;

    auto container = parameters.type.containerType();
    if (container.isEmpty())
        return MediaPlayer::SupportsType::IsNotSupported;

    std::vector<std::string> codecs;
    for (auto& codec : parameters.type.codecs())
        codecs.push_back(codec.utf8().toStdString());
    auto containerString = container.utf8().toStdString();
    auto result = MediaPlayer::SupportsType::IsNotSupported;
    // Extended parameters (width, framerate, eotf, ...): see FFmpegSupportTable.h.
    auto parameterSupport = FFmpegMedia::evaluateContentTypeParameters(FFmpegMedia::parseContentTypeParameters(parameters.type.raw().utf8().toStdString()), videoLimits());
    switch (FFmpegMedia::combineSupport(FFmpegMedia::evaluate(containerString, codecs, decoderAvailability()), parameterSupport)) {
    case FFmpegMedia::Support::IsSupported:
        result = MediaPlayer::SupportsType::IsSupported;
        break;
    case FFmpegMedia::Support::MaybeSupported:
        result = MediaPlayer::SupportsType::MayBeSupported;
        break;
    case FFmpegMedia::Support::NotSupported:
        break;
    }
    logSupportsType(parameters, result);
    return result;
}

// MARK: - Lifetime

MediaPlayerPrivateFFmpeg::MediaPlayerPrivateFFmpeg(MediaPlayer& player)
    : m_player(player)
    , m_tickTimer(RunLoop::mainSingleton(), "MediaPlayerPrivateFFmpeg::Tick"_s, this, &MediaPlayerPrivateFFmpeg::tick)
    , m_overlayTimer(RunLoop::mainSingleton(), "MediaPlayerPrivateFFmpeg::OverlayTick"_s, this, &MediaPlayerPrivateFFmpeg::overlayTick)
    , m_clock(FFmpegPlaybackClock::create())
{
}

MediaPlayerPrivateFFmpeg::~MediaPlayerPrivateFFmpeg()
{
    stopPipeline();
}

void MediaPlayerPrivateFFmpeg::stopPipeline()
{
    m_tickTimer.stop();
    m_overlayTimer.stop();
    // Order matters: the presenter reads the decoders and the overlay, the
    // sink calls into the audio decoder, the demuxer owns the packet queues
    // the decoders read (stopping it aborts them).
    stopPresenter();
    setAudioSink(nullptr);
    m_clock->setAudioDecoder(nullptr);
    if (RefPtr demuxer = m_demuxer)
        demuxer->stop();
    if (RefPtr decoder = m_videoDecoder)
        decoder->stop();
    if (RefPtr decoder = m_audioDecoder)
        decoder->stop();
    m_videoDecoder = nullptr;
    m_audioDecoder = nullptr;
    m_demuxer = nullptr;
    // The hole must not outlive the overlay.
    if (m_overlay && m_overlay->punchesHole()) {
        if (RefPtr player = m_player.get())
            player->repaint();
    }
    m_overlay = nullptr;
    m_overlayCreated = false;
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
    checkAllocatorAlignment();
    {
        auto path = m_url.path();
        size_t dot = path.reverseFind('.');
        String extension = dot != notFound && path.length() - dot <= 6 ? path.substring(dot).toString() : String();
        ffmpegMediaLog("load %s://%s (*%s), preload %s", m_url.protocol().utf8().legacyCStringPointer(), m_url.host().utf8().legacyCStringPointer(),
            extension.isEmpty() ? "" : extension.utf8().legacyCStringPointer(),
            m_preload == MediaPlayer::Preload::None ? "none" : m_preload == MediaPlayer::Preload::MetaData ? "metadata" : "auto");
    }
    m_metadataLoaded = false;
    m_failed = false;
    m_ended = false;
    m_hasVideo = m_hasAudio = false;
    m_retainedFrame = nullptr;
    m_imageFrame = nullptr;
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
    // MediaSource loads go to MediaPlayerPrivateFFmpegMSE; supportsType() never
    // selects this engine for them.
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
    ffmpegMediaLog("cancelLoad");
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
        ffmpegMediaLog("no decoder could be opened");
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
        m_clock->setAudioDecoder(decoder.copyRef());
        m_clock->setAudioSerial(m_serial);
        createAudioSink(false);
        applyGain();
        decoder->setDiscard(m_rate != 1);
    }
    m_clock->reset(0, monotonicNow());
    m_clock->setRate(m_rate, monotonicNow());
    startPresenter();

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
    setAudioSink(nullptr);
    auto render = [decoder](std::span<float> out) {
        decoder->render(out);
    };
    setAudioSink(forceNull ? AudioSink::createNull(WTF::move(render)) : AudioSink::create(WTF::move(render)));
    m_lastAudioClock.reset();
    ffmpegMediaLog("audio sink: using %s, latency %.0f ms", m_audioSink->name(), m_audioSink->latency() * 1000);
}

void MediaPlayerPrivateFFmpeg::setAudioSink(std::unique_ptr<AudioSink>&& sink)
{
    // The clock (read by the presenter thread) lets go of the old sink first.
    m_clock->setAudioSink(nullptr);
    m_audioSink = WTF::move(sink);
    m_clock->setAudioSink(m_audioSink.get());
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
    m_clock->setRunning(running, now);
    if (m_audioSink) {
        if (running && m_rate == 1)
            m_audioSink->play();
        else
            m_audioSink->pause();
    }
    if (RefPtr presenter = m_presenter)
        presenter->wake();
    if (m_demuxer)
        scheduleTick(0_s);
}

void MediaPlayerPrivateFFmpeg::setRateDouble(double rate)
{
    if (!(rate > 0) || rate == m_rate)
        return;
    m_rate = rate;
    m_clock->setRate(rate, monotonicNow());
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
    return m_clock->time(now);
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
    return m_clock->running() && !m_clock->stalled() && !m_seeking && !m_ended;
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

    ffmpegMediaLog("seek #%u to %.3f (from %.3f)", m_serial + 1, time, currentTime().toDouble());
    m_seekStartedAt = monotonicNow();
    m_seekFailureHandled = false;
    m_loggedFirstFrameSerial = 0;
    ++m_serial;
    m_seekTime = time;
    m_seeking = true;
    m_ended = false;
    // The presenter first: it must not peek the decoder with the old serial
    // once frames of the new one exist.
    if (RefPtr presenter = m_presenter) {
        presenter->setSeeking(true, time);
        presenter->setSerial(m_serial);
    }
    m_clock->setAudioSerial(m_serial);
    // Decoders next, so the first packets of the new serial are trimmed.
    if (RefPtr decoder = m_videoDecoder)
        decoder->setSeekTarget(time, m_serial);
    if (RefPtr decoder = m_audioDecoder)
        decoder->setSeekTarget(time, m_serial);
    if (m_audioSink)
        m_audioSink->flush();
    demuxer->seek(time, m_serial);
    m_clock->reset(time, monotonicNow());
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
    RefPtr frame = currentFrame();
    bool videoReady = !video || !m_pageVisible || video->hasFatalError()
        || (frame && frame->serial == m_serial) || video->reachedEnd(m_serial);
    bool audioReady = !audio || audio->hasFatalError() || audio->hasData(m_serial) || audio->drained(m_serial);
    if (!videoReady || !audioReady)
        return;

    if (m_seekFailureHandled && frame && frame->serial == m_serial)
        m_seekTime = frame->pts;
    ffmpegMediaLog("seek #%u finished at %.3f in %.0f ms", m_serial, m_seekTime, (now - m_seekStartedAt) * 1000);
    m_seeking = false;
    m_clock->reset(m_seekTime, now);
    if (RefPtr presenter = m_presenter)
        presenter->setSeeking(false, m_seekTime);
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
    // The presenter keeps the video decoder's hint current.
    if (RefPtr decoder = m_audioDecoder)
        decoder->setClockHint(position);

    if (m_seeking && status.seekFailedSerial == m_serial && !m_seekFailureHandled) {
        // The demuxer could not seek and reads on from the old position:
        // stop trimming to the target, show whatever comes next.
        m_seekFailureHandled = true;
        if (RefPtr decoder = m_videoDecoder)
            decoder->setSeekTarget(-1, m_serial);
        if (RefPtr decoder = m_audioDecoder)
            decoder->setSeekTarget(-1, m_serial);
    }
    updateOverlay();
    finishSeekIfReady(now);
    logStats(now, position, status.bytesReceived, status.readerWaiting, status.endOfStream);

    double bufferedAhead = status.buffered.bufferedAhead(m_seeking ? m_seekTime : position);
    // Extrapolated clock (no audio master): hold still while starved.
    m_clock->setStalled(!status.endOfStream && bufferedAhead < stallThreshold && !m_seeking, now);

    checkEnded(status.endOfStream, position);
    updateBuffered(status.buffered);
    updateStates(status.endOfStream, status.readerWaiting, position, bufferedAhead, status.failure);
    checkAudioWatchdog(now);

    if (m_failed)
        return;
    // Frames are the presenter's; this tick only keeps the HTML states
    // current (HTMLMediaElement reads currentTime() through the clock).
    if (m_seeking)
        scheduleTick(busyStateTick);
    else if (m_clock->running() && !m_ended)
        scheduleTick(playingStateTick);
    else
        scheduleTick(idleStateTick);
}

void MediaPlayerPrivateFFmpeg::overlayTick()
{
    updateOverlay();
    if (m_overlay && m_videoDecoder)
        m_overlayTimer.startOneShot(overlayGeometryTick);
}

void MediaPlayerPrivateFFmpeg::startPresenter()
{
    RefPtr decoder = m_videoDecoder;
    if (!decoder || m_presenter)
        return;
    m_presenter = FFmpegVideoPresenter::create(m_clock.copyRef(), decoder.releaseNonNull(), m_serial, "", [this](unsigned events) {
        handlePresenterEvents(events);
    });
    RefPtr presenter = m_presenter;
    presenter->setSeeking(m_seeking, m_seekTime);
    presenter->setOverlay(m_overlay.get());
    presenter->start();
}

void MediaPlayerPrivateFFmpeg::stopPresenter()
{
    RefPtr presenter = std::exchange(m_presenter, nullptr);
    if (!presenter)
        return;
    presenter->stop();
    m_retainedFrame = presenter->currentFrame();
    m_presenterDroppedBefore += presenter->droppedFrames();
}

bool MediaPlayerPrivateFFmpeg::hasAvailableVideoFrame() const
{
    return !!currentFrame();
}

RefPtr<FFmpegVideoFrame> MediaPlayerPrivateFFmpeg::currentFrame() const
{
    if (RefPtr presenter = m_presenter)
        return presenter->currentFrame();
    return m_retainedFrame;
}

// Main thread, coalesced: the presenter thread showed one or more frames.
void MediaPlayerPrivateFFmpeg::handlePresenterEvents(unsigned events)
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
            ffmpegMediaLog("first video frame%s: pts %.3f, %dx%d", frame->serial ? " after seek" : "", frame->pts, frame->width(), frame->height());
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

void MediaPlayerPrivateFFmpeg::updateOverlay()
{
    if (!m_hasVideo || !m_videoDecoder)
        return;
    if (!std::exchange(m_overlayCreated, true)) {
        m_overlay = FFmpegVideoOverlay::createIfEnabled();
        if (RefPtr presenter = m_presenter)
            presenter->setOverlay(m_overlay.get());
        if (m_overlay)
            m_overlayTimer.startOneShot(overlayGeometryTick);
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

void MediaPlayerPrivateFFmpeg::checkEnded(bool endOfStream, double position)
{
    if (m_ended || m_seeking || !endOfStream || !m_clock->running())
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
        ready.currentDataAvailable = [&] {
            RefPtr frame = currentFrame();
            return frame && frame->serial == m_serial;
        }();
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
    ffmpegMediaLog("audio sink %s stopped pulling samples for %.0f s, switching to the null sink", m_audioSink->name(), audioWatchdogSeconds);
    createAudioSink(true);
    updatePlaybackRunning();
}

void MediaPlayerPrivateFFmpeg::logStats(double now, double position, uint64_t bytesReceived, bool readerWaiting, bool endOfStream)
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
    RefPtr demuxer = m_demuxer;
    RefPtr video = m_videoDecoder;
    RefPtr audio = m_audioDecoder;
    auto videoLevel = demuxer ? demuxer->videoQueue().level() : FFmpegMedia::QueueLevel { };
    auto audioLevel = demuxer ? demuxer->audioQueue().level() : FFmpegMedia::QueueLevel { };
    std::optional<double> audioClock;
    if (audio && m_audioSink)
        audioClock = audio->clockTime(m_audioSink->latency());
    ffmpegMediaLog("stats: t=%.2f%s video %u decoded %u dropped (+%u presenter) %zu queued, pts %.2f | packets v %zu/%.1fs a %zu/%.1fs %zu KiB | audio %s clock %.2f buffered %.2fs | net %llu KiB%s%s",
        position, m_seeking ? makeString(" SEEKING #"_s, m_serial, " for "_s, static_cast<int>((now - m_seekStartedAt) * 1000), "ms"_s).utf8().legacyCStringPointer() : "",
        video ? video->decodedFrames() : 0, video ? video->droppedFrames() : 0, presenterDroppedFrames(), video ? video->queuedFrames() : 0,
        currentFrame() ? currentFrame()->pts : -1.0,
        videoLevel.packets, videoLevel.seconds, audioLevel.packets, audioLevel.seconds,
        demuxer ? (demuxer->videoQueue().bytes() + demuxer->audioQueue().bytes()) >> 10 : 0,
        m_audioSink ? m_audioSink->name() : "none", audioClock ? *audioClock : -1.0, audio ? audio->bufferedSeconds() : 0.0,
        static_cast<unsigned long long>(bytesReceived >> 10), readerWaiting ? " reader idle" : "", endOfStream ? " EOS" : "");
    if (m_overlay)
        m_overlay->logStats(statsIntervalSeconds);
}

// MARK: - States

void MediaPlayerPrivateFFmpeg::setNetworkState(MediaPlayer::NetworkState state)
{
    if (m_networkState == state)
        return;
    ffmpegMediaLog("networkState %s -> %s", networkStateName(m_networkState), networkStateName(state));
    m_networkState = state;
    if (RefPtr player = m_player.get())
        player->networkStateChanged();
}

void MediaPlayerPrivateFFmpeg::setReadyState(MediaPlayer::ReadyState state)
{
    if (m_readyState == state)
        return;
    ffmpegMediaLog("readyState %s -> %s", readyStateName(m_readyState), readyStateName(state));
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
    return (decoder ? decoder->droppedFrames() : 0) + presenterDroppedFrames();
}

// MARK: - Painting

RefPtr<NativeImage> MediaPlayerPrivateFFmpeg::currentNativeImage()
{
    RefPtr frame = currentFrame();
    if (frame != m_imageFrame) {
        m_imageFrame = frame;
        m_currentImage = frame ? createNativeImage(*frame) : nullptr;
    }
    return m_currentImage;
}

RefPtr<NativeImage> MediaPlayerPrivateFFmpeg::createNativeImage(FFmpegVideoFrame& videoFrame)
{
    // The SkImage wraps the frame's pixels without copying and keeps the
    // frame alive; the decoder's pool only reuses frames nobody references.
    Ref frame = videoFrame;
    auto info = SkImageInfo::Make(frame->width(), frame->height(), kBGRA_8888_SkColorType, kOpaque_SkAlphaType, SkColorSpace::MakeSRGB());
    SkPixmap pixmap(info, frame->data(), frame->stride());
    auto image = SkImages::RasterFromPixmap(pixmap, [](const void*, void* context) {
        static_cast<FFmpegVideoFrame*>(context)->deref();
    }, frame.ptr());
    if (!image)
        return nullptr;
    // Owned by the SkImage from now on (released by the callback above).
    (void)frame.leakRef();
    return NativeImage::create(WTF::move(image));
}

void MediaPlayerPrivateFFmpeg::paintVideo(GraphicsContext& context, const FloatRect& rect, FFmpegVideoOverlay* overlay, bool forElement, const RefPtr<NativeImage>& image)
{
    if (context.paintingDisabled() || rect.isEmpty())
        return;
    if (forElement && overlay && overlay->punchesHole()) {
        // Transparent pixels: the UI composes the overlay under the page.
        context.clearRect(rect);
        return;
    }
    if (!image)
        return;
    context.drawNativeImage(*image, rect, FloatRect { { }, image->size() });
}

void MediaPlayerPrivateFFmpeg::paint(GraphicsContext& context, const FloatRect& rect)
{
    paintVideo(context, rect, m_overlay.get(), true, m_overlay && m_overlay->punchesHole() ? nullptr : currentNativeImage());
}

void MediaPlayerPrivateFFmpeg::paintCurrentFrameInContext(GraphicsContext& context, const FloatRect& rect)
{
    // Canvas drawImage() and snapshots need the pixels, never the hole.
    paintVideo(context, rect, nullptr, false, currentNativeImage());
}

RefPtr<NativeImage> MediaPlayerPrivateFFmpeg::nativeImageForCurrentTime()
{
    return currentNativeImage();
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
