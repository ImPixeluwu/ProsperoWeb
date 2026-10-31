// SPDX-License-Identifier: GPL-3.0-or-later
//
// MediaPlayerPrivate backed by ffmpeg (libavformat/libavcodec/libswscale/
// libswresample) with software decoding, for ports without a platform media
// stack (the PS5 homebrew build). Plays progressive MP4/WebM/MP3/Ogg/FLAC/
// WAV and HLS (libavformat's hls demuxer); MSE is MediaPlayerPrivateFFmpegMSE
// (mse/). No EME, WebAudio or hardware decoding. Video is painted from BGRA frames through NativeImage,
// audio goes to an AudioSink (SDL2, or a silent fallback).
//
// Threads: main (this object, a low-rate state tick), demuxer, video
// decoder, video presenter (FFmpegVideoPresenter.h), audio decoder, audio
// sink. See webkit-port/media/README.md.

#pragma once

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "ColorSpace.h"
#include "FFmpegAVClock.h"
#include "FFmpegPlaybackPolicy.h"
#include "FFmpegSupportTable.h"
#include "FFmpegTimeRanges.h"
#include "FFmpegVideoPresenter.h"
#include "MediaPlayerPrivate.h"
#include <memory>
#include <optional>
#include <wtf/MonotonicTime.h>
#include <wtf/RefCounted.h>
#include <wtf/RunLoop.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/URL.h>
#include <wtf/WeakPtr.h>

namespace WebCore {

class AudioSink;
class FFmpegAudioDecoder;
class FFmpegDemuxer;
class FFmpegVideoDecoder;
class FFmpegVideoFrame;
class FFmpegVideoOverlay;

class MediaPlayerPrivateFFmpeg final
    : public MediaPlayerPrivateInterface
    , public CanMakeWeakPtr<MediaPlayerPrivateFFmpeg>
    , public RefCounted<MediaPlayerPrivateFFmpeg> {
    WTF_MAKE_TZONE_ALLOCATED(MediaPlayerPrivateFFmpeg);
public:
    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    explicit MediaPlayerPrivateFFmpeg(MediaPlayer&);
    ~MediaPlayerPrivateFFmpeg();

    constexpr MediaPlayerType mediaPlayerType() const final { return MediaPlayerType::FFmpeg; }

    static void registerMediaEngine(MediaEngineRegistrar);
    static void getSupportedTypes(HashSet<String>&);
    static MediaPlayer::SupportsType supportsType(const MediaEngineSupportParameters&);

    // Shared with the MSE player (MediaPlayerPrivateFFmpegMSE).
    // VP8/VP9 count as unavailable unless PROSPEROWEB_MEDIA_VP9=1.
    static const FFmpegMedia::DecoderAvailability& decoderAvailability();
    // 1080p unless PROSPEROWEB_MEDIA_MAX_VIDEO=WxH.
    static const FFmpegMedia::VideoLimits& videoLimits();
    static void logSupportsType(const MediaEngineSupportParameters&, MediaPlayer::SupportsType);
    // Wraps the frame's pixels without copying; the image keeps the frame alive.
    static RefPtr<NativeImage> createNativeImage(FFmpegVideoFrame&);
    // Shared painting: a hole while `overlay` shows the video (only for the
    // element's own painting, `forElement`), otherwise the frame.
    static void paintVideo(GraphicsContext&, const FloatRect&, FFmpegVideoOverlay*, bool forElement, const RefPtr<NativeImage>&);
    // Main tick periods shared with the MSE player: the presenter thread
    // shows the frames, the tick only keeps the HTML states current.
    static constexpr Seconds playingStateTick = 250_ms;
    static constexpr Seconds busyStateTick = 50_ms;
    static constexpr Seconds idleStateTick = 100_ms;
    // The overlay's geometry follows layout and scrolling at this rate.
    static constexpr Seconds overlayGeometryTick = 33_ms;

    void load(const String&) final;
#if ENABLE(MEDIA_SOURCE)
    void load(const URL&, const LoadOptions&, MediaSourcePrivateClient&) final;
#endif
#if ENABLE(MEDIA_STREAM)
    void load(MediaStreamPrivate&) final;
#endif
    void cancelLoad() final;
    void prepareToPlay() final;
    void setPreload(MediaPlayer::Preload) final;

    void play() final;
    void pause() final;
    bool paused() const final { return m_paused; }
    bool ended() const final { return m_ended; }

    FloatSize naturalSize() const final { return m_naturalSize; }
    bool hasVideo() const final { return m_hasVideo; }
    bool hasAudio() const final { return m_hasAudio; }
    void setPageIsVisible(bool) final;

    MediaTime duration() const final;
    MediaTime currentTime() const final;
    bool timeIsProgressing() const final;
    Ref<MediaTimePromise> seekToTarget(const SeekTarget&) final;
    MediaTime maxTimeSeekable() const final;
    MediaTime minTimeSeekable() const final { return MediaTime::zeroTime(); }

    void setRate(float rate) final { setRateDouble(rate); }
    void setRateDouble(double) final;
    double rate() const final { return m_rate; }
    void setVolume(float) final;
    void setMuted(bool) final;

    MediaPlayer::NetworkState networkState() const final { return m_networkState; }
    MediaPlayer::ReadyState readyState() const final { return m_readyState; }
    const PlatformTimeRanges& buffered() const final { return m_buffered; }
    bool didLoadingProgress() const final;
    MediaPlayer::MovieLoadType movieLoadType() const final;
    bool didPassCORSAccessCheck() const final;

    void paint(GraphicsContext&, const FloatRect&) final;
    void paintCurrentFrameInContext(GraphicsContext&, const FloatRect&) final;
    RefPtr<NativeImage> nativeImageForCurrentTime() final;
    ColorSpace colorSpace() final { return ColorSpace::SRGB(); }
    bool hasAvailableVideoFrame() const final;

    bool supportsFullscreen() const final { return true; }
    unsigned decodedFrameCount() const final;
    unsigned droppedFrameCount() const final;
    String engineDescription() const final { return "FFmpeg"_s; }

private:
    void startLoading();
    void stopPipeline();
    void onMetadataLoaded();
    void createAudioSink(bool forceNull);

    void tick();
    void scheduleTick(Seconds);
    double clockTime(double now) const;
    void startPresenter();
    void stopPresenter();
    void handlePresenterEvents(unsigned);
    RefPtr<FFmpegVideoFrame> currentFrame() const;
    void overlayTick();
    unsigned presenterDroppedFrames() const { return m_presenterDroppedBefore + (m_presenter ? m_presenter->droppedFrames() : 0); }
    void finishSeekIfReady(double now);
    void checkEnded(bool endOfStream, double position);
    void updateBuffered(const FFmpegMedia::TimeRangeSet&);
    void updateStates(bool endOfStream, bool readerWaiting, double position, double bufferedAhead, FFmpegMedia::LoadFailure);
    void updatePlaybackRunning();
    void checkAudioWatchdog(double now);
    void logStats(double now, double position, uint64_t bytesReceived, bool readerWaiting, bool endOfStream);
    void applyGain();
    void updateOverlay();
    // Creates the video overlay once (before the video decoder, which may
    // ask the UI to decode through it).
    void createOverlayIfNeeded();
    void setAudioSink(std::unique_ptr<AudioSink>&&);

    void setNetworkState(MediaPlayer::NetworkState);
    void setReadyState(MediaPlayer::ReadyState);
    RefPtr<NativeImage> currentNativeImage();

    ThreadSafeWeakPtr<MediaPlayer> m_player;
    RunLoop::Timer m_tickTimer;
    RunLoop::Timer m_overlayTimer;
    URL m_url;

    RefPtr<FFmpegDemuxer> m_demuxer;
    RefPtr<FFmpegVideoDecoder> m_videoDecoder;
    RefPtr<FFmpegAudioDecoder> m_audioDecoder;
    std::unique_ptr<AudioSink> m_audioSink;
    const Ref<FFmpegPlaybackClock> m_clock;
    RefPtr<FFmpegVideoPresenter> m_presenter;

    MediaPlayer::NetworkState m_networkState { MediaPlayer::NetworkState::Empty };
    MediaPlayer::ReadyState m_readyState { MediaPlayer::ReadyState::HaveNothing };
    MediaPlayer::Preload m_preload { MediaPlayer::Preload::Auto };
    PlatformTimeRanges m_buffered;
    mutable uint64_t m_lastProgressBytes { 0 };

    // The frame on screen after the presenter stopped (cancelLoad).
    RefPtr<FFmpegVideoFrame> m_retainedFrame;
    // currentNativeImage() cache, valid for m_imageFrame.
    RefPtr<FFmpegVideoFrame> m_imageFrame;
    RefPtr<NativeImage> m_currentImage;
    FloatSize m_naturalSize;
    // Video overlay (FFmpegVideoOverlay.h), created with the video pipeline.
    std::unique_ptr<FFmpegVideoOverlay> m_overlay;
    bool m_overlayCreated { false };

    std::optional<MediaTimePromise::AutoRejectProducer> m_seekPromise;
    double m_seekTime { 0 };
    double m_endTime { 0 };
    double m_volume { 1 };
    double m_rate { 1 };
    unsigned m_serial { 0 };
    // Frames the presenters of earlier pipelines dropped.
    unsigned m_presenterDroppedBefore { 0 };

    // Audio sink watchdog: a device that opens but never pulls data would
    // freeze the audio clock forever.
    std::optional<double> m_lastAudioClock;
    double m_lastAudioClockChange { 0 };

    bool m_loadStarted { false };
    bool m_loadDeferred { false };
    bool m_metadataLoaded { false };
    bool m_hasVideo { false };
    bool m_hasAudio { false };
    bool m_paused { true };
    bool m_ended { false };
    bool m_seeking { false };
    bool m_muted { false };
    bool m_pageVisible { true };
    bool m_isLive { false };
    bool m_isSeekable { false };
    bool m_failed { false };
    bool m_seekFailureHandled { false };
    double m_duration { 0 };
    // Console log bookkeeping.
    double m_seekStartedAt { 0 };
    double m_lastStatsAt { 0 };
    unsigned m_loggedFirstFrameSerial { 0 };
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
