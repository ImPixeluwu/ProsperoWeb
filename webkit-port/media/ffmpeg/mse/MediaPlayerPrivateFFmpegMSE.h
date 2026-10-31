// SPDX-License-Identifier: GPL-3.0-or-later
//
// Media Source Extensions player of the ffmpeg media engine. Samples come
// from SourceBufferPrivateFFmpeg (the generic SourceBufferPrivate code
// decides what to enqueue: from the sync sample before the playback
// position, re-enqueued after seeks and overlapping appends) and go to one
// FFmpegPacketQueue per selected video/audio track, read by the same
// FFmpegVideoDecoder/FFmpegAudioDecoder, AVClock, AudioSink and frame
// painting the progressive MediaPlayerPrivateFFmpeg uses. readyState is the
// MediaSource's (computed from buffered ranges), held at HAVE_METADATA until
// the frame at the position is on screen. Main thread, except that the
// FFmpegVideoPresenter thread shows the frames.

#pragma once

#if ENABLE(VIDEO) && USE(FFMPEG) && ENABLE(MEDIA_SOURCE)

#include "FFmpegAVClock.h"
#include "FFmpegUtilities.h"
#include "FFmpegVideoPresenter.h"
#include "MediaPlayerPrivate.h"
#include <memory>
#include <optional>
#include <wtf/RefCounted.h>
#include <wtf/RunLoop.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeWeakPtr.h>
#include <wtf/WeakPtr.h>

namespace WebCore {

class AudioSink;
class FFmpegAudioDecoder;
class FFmpegMSETrackFormat;
class FFmpegPacketQueue;
class FFmpegVideoDecoder;
class FFmpegVideoFrame;
class FFmpegVideoOverlay;
class MediaSampleFFmpeg;
class MediaSourcePrivateFFmpeg;
class SourceBufferPrivateFFmpeg;

class MediaPlayerPrivateFFmpegMSE final
    : public MediaPlayerPrivateInterface
    , public CanMakeWeakPtr<MediaPlayerPrivateFFmpegMSE>
    , public RefCounted<MediaPlayerPrivateFFmpegMSE> {
    WTF_MAKE_TZONE_ALLOCATED(MediaPlayerPrivateFFmpegMSE);
public:
    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    explicit MediaPlayerPrivateFFmpegMSE(MediaPlayer&);
    ~MediaPlayerPrivateFFmpegMSE();

    constexpr MediaPlayerType mediaPlayerType() const final { return MediaPlayerType::FFmpegMSE; }

    static void registerMediaEngine(MediaEngineRegistrar);
    static void getSupportedTypes(HashSet<String>&);
    static MediaPlayer::SupportsType supportsType(const MediaEngineSupportParameters&);

    // MediaSourcePrivateFFmpeg / SourceBufferPrivateFFmpeg.
    void enqueueSample(SourceBufferPrivateFFmpeg&, TrackID, MediaSampleFFmpeg&);
    bool isReadyForMoreSamples(SourceBufferPrivateFFmpeg&, TrackID);
    void notifyWhenReadyForMoreSamples(SourceBufferPrivateFFmpeg&, TrackID);
    void flushTrack(SourceBufferPrivateFFmpeg&, TrackID);
    void trackEnded(SourceBufferPrivateFFmpeg&, TrackID);
    void sourceBufferRemoved(SourceBufferPrivateFFmpeg&);
    void videoFormatAvailable(const FFmpegMSETrackFormat&);
    void didAppendBytes(size_t);
    void durationChangedFromMediaSource();
    void endOfStreamCleared();
    void activeSourceBuffersChanged();

#if !RELEASE_LOG_DISABLED
    uint64_t mediaPlayerLogIdentifier() { return m_player.get()->mediaPlayerLogIdentifier(); }
    const Logger& mediaPlayerLogger() { return m_player.get()->mediaPlayerLogger(); }
#endif

private:
    void load(const String&) final;
    void load(const URL&, const LoadOptions&, MediaSourcePrivateClient&) final;
#if ENABLE(MEDIA_STREAM)
    void load(MediaStreamPrivate&) final;
#endif
    void cancelLoad() final;

    void play() final;
    void pause() final;
    bool paused() const final { return m_paused; }
    bool ended() const final { return m_ended; }

    FloatSize naturalSize() const final { return m_naturalSize; }
    bool hasVideo() const final;
    bool hasAudio() const final;
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
    void readyStateFromMediaSourceChanged() final;
    void characteristicsFromMediaSourceChanged() final;
    void seekableRangesFromMediaSourceChanged() final;
    void mediaSourceHasRetrievedAllData() final;
    const PlatformTimeRanges& buffered() const final;
    bool didLoadingProgress() const final;
    MediaPlayer::MovieLoadType movieLoadType() const final { return MediaPlayer::MovieLoadType::StoredStream; }
    bool didPassCORSAccessCheck() const final { return true; }

    void paint(GraphicsContext&, const FloatRect&) final;
    void paintCurrentFrameInContext(GraphicsContext&, const FloatRect&) final;
    RefPtr<NativeImage> nativeImageForCurrentTime() final;
    ColorSpace colorSpace() final;
    bool hasAvailableVideoFrame() const final;

    bool supportsFullscreen() const final { return true; }
    unsigned decodedFrameCount() const final;
    unsigned droppedFrameCount() const final;
    std::optional<VideoPlaybackQualityMetrics> videoPlaybackQualityMetrics() final;
    String engineDescription() const final { return "FFmpeg MSE"_s; }

    // One selected track feeding a decoder.
    struct Pipeline {
        bool isVideo { false };
        ThreadSafeWeakPtr<SourceBufferPrivateFFmpeg> sourceBuffer;
        const SourceBufferPrivateFFmpeg* sourceBufferKey { nullptr };
        TrackID trackID { 0 };
        RefPtr<FFmpegMSETrackFormat> decoderFormat; // configuration the decoder runs with
        std::unique_ptr<FFmpegPacketQueue> queue;
        unsigned serial { 0 };
        bool wantsMore { false };
        bool endOfStream { false };
        bool failed { false };
        uint64_t enqueued { 0 };
        double lastEnqueuedEnd { 0 };

        bool bound() const { return !!sourceBufferKey; }
        bool matches(const SourceBufferPrivateFFmpeg& buffer, TrackID id) const { return sourceBufferKey == &buffer && trackID == id; }
    };
    Pipeline* pipelineFor(SourceBufferPrivateFFmpeg&, TrackID);
    bool createPipeline(Pipeline&, SourceBufferPrivateFFmpeg&, TrackID, const FFmpegMSETrackFormat&);
    void flushPipeline(Pipeline&, double target);
    void stopPipelines();
    void createAudioSink(bool forceNull);
    void applyGain();
    void updateOverlay();
    void overlayTick();
    void setAudioSink(std::unique_ptr<AudioSink>&&);
    void startPresenter();
    void stopPresenter();
    void handlePresenterEvents(unsigned);
    RefPtr<FFmpegVideoFrame> currentFrame() const;
    unsigned presenterDroppedFrames() const { return m_presenterDroppedBefore + (m_presenter ? m_presenter->droppedFrames() : 0); }

    void tick();
    void scheduleTick(Seconds);
    double clockTime(double now) const;
    double playbackPosition() const;
    void finishSeekIfReady(double now);
    void checkEnded(double position);
    void updateReadyState();
    void updatePlaybackRunning();
    void checkAudioWatchdog(double now);
    void logStats(double now, double position);

    void setNetworkState(MediaPlayer::NetworkState);
    void setReadyState(MediaPlayer::ReadyState);
    RefPtr<NativeImage> currentNativeImage();

    ThreadSafeWeakPtr<MediaPlayer> m_player;
    RunLoop::Timer m_tickTimer;
    RunLoop::Timer m_overlayTimer;
    RefPtr<MediaSourcePrivateFFmpeg> m_mediaSourcePrivate;

    Pipeline m_video;
    Pipeline m_audio;
    RefPtr<FFmpegVideoDecoder> m_videoDecoder;
    RefPtr<FFmpegAudioDecoder> m_audioDecoder;
    std::unique_ptr<AudioSink> m_audioSink;
    const Ref<FFmpegPlaybackClock> m_clock;
    RefPtr<FFmpegVideoPresenter> m_presenter;

    MediaPlayer::NetworkState m_networkState { MediaPlayer::NetworkState::Empty };
    MediaPlayer::ReadyState m_readyState { MediaPlayer::ReadyState::HaveNothing };

    // The frame on screen after the presenter stopped.
    RefPtr<FFmpegVideoFrame> m_retainedFrame;
    // currentNativeImage() cache, valid for m_imageFrame.
    RefPtr<FFmpegVideoFrame> m_imageFrame;
    RefPtr<NativeImage> m_currentImage;
    // Video overlay (FFmpegVideoOverlay.h), created with the video decoder.
    std::unique_ptr<FFmpegVideoOverlay> m_overlay;
    bool m_overlayCreated { false };
    FloatSize m_naturalSize;

    std::optional<MediaTimePromise::AutoRejectProducer> m_seekPromise;
    unsigned m_seekGeneration { 0 };
    bool m_seeking { false };
    // The seek's samples were re-enqueued; waiting for the first frame.
    bool m_seekReenqueued { false };
    double m_seekTime { 0 };
    double m_endTime { 0 };
    double m_volume { 1 };
    double m_rate { 1 };
    unsigned m_presenterDroppedBefore { 0 };

    std::optional<double> m_lastAudioClock;
    double m_lastAudioClockChange { 0 };

    uint64_t m_bytesAppended { 0 };
    mutable uint64_t m_lastProgressBytes { 0 };
    bool m_paused { true };
    bool m_ended { false };
    bool m_muted { false };
    bool m_pageVisible { true };
    bool m_stalled { false };
    // Console log bookkeeping.
    double m_seekStartedAt { 0 };
    double m_seekAnnouncedAt { 0 };
    double m_lastSeekWaitLog { 0 };
    double m_lastStatsAt { 0 };
    unsigned m_loggedFirstFrameSerial { 0 };
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG) && ENABLE(MEDIA_SOURCE)
