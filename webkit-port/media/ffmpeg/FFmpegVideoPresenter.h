// SPDX-License-Identifier: GPL-3.0-or-later
//
// Video presentation off the main thread for both ffmpeg players.
//
// The players used to pick and show frames from an 8 ms RunLoop timer on
// the WebProcess main thread. On the console that thread also lays out and
// paints the page, and while YouTube repainted its UI the tick was starved:
// 26 frames per second were decoded but 10 reached the screen. Now:
//
//  - FFmpegPlaybackClock is the player's AVClock behind a lock, with the
//    audio master (decoder + sink) attached, so any thread can read the
//    playback position.
//  - FFmpegVideoPresenter runs one thread per video pipeline. It reads the
//    clock, takes the due frame from the decoder's output queue
//    (FFmpegPresenterPolicy.h: drop only beyond 1.5 frame intervals late),
//    and hands it to the video overlay itself, which copies it to the UI
//    process (shm/file/tcp). It sleeps until the next frame is due.
//  - The main thread hears about presented frames through coalesced
//    callOnMainThread() events: a repaint when the page paints the video
//    (overlay off or not punching), and the first frame of each position
//    (seek completion, readyState, firstVideoFrameAvailable). Everything else
//    (readyState, ended, buffered) stays on the player's low-rate tick.
//
// Threading: create/start/stop/set* on the main thread, currentFrame() and
// droppedFrames() from any thread, events are delivered on the main thread
// until stop().

#pragma once

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "FFmpegAVClock.h"
#include "FFmpegPresenterPolicy.h"
#include <atomic>
#include <optional>
#include <wtf/Condition.h>
#include <wtf/Function.h>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Threading.h>

namespace WebCore {

class AudioSink;
class FFmpegAudioDecoder;
class FFmpegVideoDecoder;
class FFmpegVideoFrame;
class FFmpegVideoOverlay;

class FFmpegPlaybackClock final : public ThreadSafeRefCounted<FFmpegPlaybackClock> {
public:
    static Ref<FFmpegPlaybackClock> create();
    ~FFmpegPlaybackClock();

    void reset(double mediaTime, double now);
    void setRunning(bool, double now);
    void setStalled(bool, double now);
    void setRate(double, double now);
    double rate() const;
    bool running() const;
    bool stalled() const;

    // The audio master. The sink pointer must be cleared (setAudioSink(nullptr))
    // before the sink is destroyed; `serial` is the audio pipeline's serial.
    void setAudioDecoder(RefPtr<FFmpegAudioDecoder>&&);
    void setAudioSink(AudioSink*);
    void setAudioSerial(unsigned);

    // Media time now: the audio position while audio drives the clock,
    // otherwise extrapolated (FFmpegAVClock.h).
    double time(double now);

private:
    FFmpegPlaybackClock();

    mutable Lock m_lock;
    FFmpegMedia::AVClock m_clock WTF_GUARDED_BY_LOCK(m_lock);
    RefPtr<FFmpegAudioDecoder> m_audioDecoder WTF_GUARDED_BY_LOCK(m_lock);
    AudioSink* m_audioSink WTF_GUARDED_BY_LOCK(m_lock) { nullptr };
    unsigned m_audioSerial WTF_GUARDED_BY_LOCK(m_lock) { 0 };
};

class FFmpegVideoPresenter final : public ThreadSafeRefCounted<FFmpegVideoPresenter> {
public:
    enum Event : unsigned {
        // A frame was presented: repaint the element unless the overlay
        // shows it.
        FrameShown = 1 << 0,
        // The first frame of the current serial was presented.
        FirstFrame = 1 << 1,
        // The decoder drained the stream for the current serial.
        ReachedEnd = 1 << 2,
    };
    using EventHandler = Function<void(unsigned events)>;

    // `logTag` is appended to the "presenter:" log line (" [mse]").
    static Ref<FFmpegVideoPresenter> create(Ref<FFmpegPlaybackClock>&&, Ref<FFmpegVideoDecoder>&&, unsigned serial, const char* logTag, EventHandler&&);
    ~FFmpegVideoPresenter();

    void start();
    // Joins the thread; no event is delivered afterwards.
    void stop();

    // The overlay receives presented frames; nullptr detaches it. The
    // overlay must outlive the presenter or be detached first.
    void setOverlay(FFmpegVideoOverlay*);
    // Sends the current frame to the overlay again (the hole was just punched).
    void resubmitToOverlay();
    // A seek or flush moved the decoder to `serial`.
    void setSerial(unsigned);
    // While seeking the position is the seek target, and the first frame of
    // the new serial is shown as soon as it is decoded.
    void setSeeking(bool seeking, double seekTime);
    // The clock started, stopped or was reset: re-evaluate now.
    void wake();

    RefPtr<FFmpegVideoFrame> currentFrame() const;
    unsigned droppedFrames() const { return m_dropped; }

private:
    FFmpegVideoPresenter(Ref<FFmpegPlaybackClock>&&, Ref<FFmpegVideoDecoder>&&, unsigned serial, const char* logTag, EventHandler&&);

    void run();
    // One pass: presents what is due, returns the seconds to sleep.
    double present(double now);
    void post(unsigned events);
    void deliver();

    const Ref<FFmpegPlaybackClock> m_clock;
    const Ref<FFmpegVideoDecoder> m_decoder;
    const char* const m_logTag;
    RefPtr<Thread> m_thread;
    // Main thread only.
    EventHandler m_handler;

    mutable Lock m_lock;
    Condition m_condition;
    bool m_stopping WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_woken WTF_GUARDED_BY_LOCK(m_lock) { false };
    unsigned m_serial WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    bool m_seeking WTF_GUARDED_BY_LOCK(m_lock) { false };
    double m_seekTime WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    RefPtr<FFmpegVideoFrame> m_current WTF_GUARDED_BY_LOCK(m_lock);
    FFmpegVideoOverlay* m_overlay WTF_GUARDED_BY_LOCK(m_lock) { nullptr };

    // Presenter thread only.
    FFmpegMedia::PresenterStats m_stats;
    unsigned m_endSerialPosted { 0 };
    bool m_endPosted { false };

    std::atomic<unsigned> m_dropped { 0 };
    std::atomic<unsigned> m_pendingEvents { 0 };
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
