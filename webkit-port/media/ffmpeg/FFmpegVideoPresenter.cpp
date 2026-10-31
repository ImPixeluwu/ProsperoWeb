// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "FFmpegVideoPresenter.h"

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "AudioSink.h"
#include "FFmpegAudioDecoder.h"
#include "FFmpegLog.h"
#include "FFmpegVideoDecoder.h"
#include "FFmpegVideoOverlay.h"
#include <wtf/MainThread.h>
#include <wtf/MonotonicTime.h>

namespace WebCore {

namespace {

constexpr double statsIntervalSeconds = 5.0;

double monotonicNow()
{
    return MonotonicTime::now().secondsSinceEpoch().seconds();
}

} // namespace

// MARK: - FFmpegPlaybackClock

Ref<FFmpegPlaybackClock> FFmpegPlaybackClock::create()
{
    return adoptRef(*new FFmpegPlaybackClock);
}

FFmpegPlaybackClock::FFmpegPlaybackClock() = default;
FFmpegPlaybackClock::~FFmpegPlaybackClock() = default;

void FFmpegPlaybackClock::reset(double mediaTime, double now)
{
    Locker locker { m_lock };
    m_clock.reset(mediaTime, now);
}

void FFmpegPlaybackClock::setRunning(bool running, double now)
{
    Locker locker { m_lock };
    m_clock.setRunning(running, now);
}

void FFmpegPlaybackClock::setStalled(bool stalled, double now)
{
    Locker locker { m_lock };
    m_clock.setStalled(stalled, now);
}

void FFmpegPlaybackClock::setRate(double rate, double now)
{
    Locker locker { m_lock };
    m_clock.setRate(rate, now);
}

double FFmpegPlaybackClock::rate() const
{
    Locker locker { m_lock };
    return m_clock.rate();
}

bool FFmpegPlaybackClock::running() const
{
    Locker locker { m_lock };
    return m_clock.running();
}

bool FFmpegPlaybackClock::stalled() const
{
    Locker locker { m_lock };
    return m_clock.stalled();
}

void FFmpegPlaybackClock::setAudioDecoder(RefPtr<FFmpegAudioDecoder>&& decoder)
{
    Locker locker { m_lock };
    m_audioDecoder = WTF::move(decoder);
}

void FFmpegPlaybackClock::setAudioSink(AudioSink* sink)
{
    Locker locker { m_lock };
    m_audioSink = sink;
}

void FFmpegPlaybackClock::setAudioSerial(unsigned serial)
{
    Locker locker { m_lock };
    m_audioSerial = serial;
}

double FFmpegPlaybackClock::time(double now)
{
    // Lock order: this lock, then the sink's and the decoder's (taken one
    // after the other, never nested); nothing takes this one under them.
    Locker locker { m_lock };
    std::optional<double> audioTime;
    if (RefPtr decoder = m_audioDecoder; decoder && m_audioSink && !decoder->drained(m_audioSerial)) {
        double latency = m_audioSink->latency();
        audioTime = decoder->clockTime(latency);
    }
    return m_clock.time(now, audioTime);
}

// MARK: - FFmpegVideoPresenter

Ref<FFmpegVideoPresenter> FFmpegVideoPresenter::create(Ref<FFmpegPlaybackClock>&& clock, Ref<FFmpegVideoDecoder>&& decoder, unsigned serial, const char* logTag, EventHandler&& handler)
{
    return adoptRef(*new FFmpegVideoPresenter(WTF::move(clock), WTF::move(decoder), serial, logTag, WTF::move(handler)));
}

FFmpegVideoPresenter::FFmpegVideoPresenter(Ref<FFmpegPlaybackClock>&& clock, Ref<FFmpegVideoDecoder>&& decoder, unsigned serial, const char* logTag, EventHandler&& handler)
    : m_clock(WTF::move(clock))
    , m_decoder(WTF::move(decoder))
    , m_logTag(logTag)
    , m_handler(WTF::move(handler))
{
    Locker locker { m_lock };
    m_serial = serial;
}

FFmpegVideoPresenter::~FFmpegVideoPresenter()
{
    ASSERT(!m_thread);
}

void FFmpegVideoPresenter::start()
{
    if (m_thread)
        return;
    m_thread = Thread::create("FFmpegPresenter"_s, [protectedThis = Ref { *this }] {
        protectedThis->run();
    }, ThreadType::Graphics, Thread::QOS::UserInteractive);
}

void FFmpegVideoPresenter::stop()
{
    m_handler = nullptr;
    {
        Locker locker { m_lock };
        m_stopping = true;
        m_overlay = nullptr;
        m_condition.notifyAll();
    }
    if (RefPtr thread = std::exchange(m_thread, nullptr))
        thread->waitForCompletion();
}

void FFmpegVideoPresenter::setOverlay(FFmpegVideoOverlay* overlay)
{
    Locker locker { m_lock };
    m_overlay = overlay;
}

void FFmpegVideoPresenter::resubmitToOverlay()
{
    Locker locker { m_lock };
    if (m_overlay && m_current)
        m_overlay->submit(*m_current);
}

void FFmpegVideoPresenter::setSerial(unsigned serial)
{
    // Taken while present() runs, so the presenter never peeks the decoder
    // with a stale serial (peekFrame() discards frames of other serials).
    Locker locker { m_lock };
    m_serial = serial;
    m_woken = true;
    m_condition.notifyAll();
}

void FFmpegVideoPresenter::setSeeking(bool seeking, double seekTime)
{
    Locker locker { m_lock };
    m_seeking = seeking;
    m_seekTime = seekTime;
    m_woken = true;
    m_condition.notifyAll();
}

void FFmpegVideoPresenter::wake()
{
    Locker locker { m_lock };
    m_woken = true;
    m_condition.notifyAll();
}

RefPtr<FFmpegVideoFrame> FFmpegVideoPresenter::currentFrame() const
{
    Locker locker { m_lock };
    return m_current;
}

void FFmpegVideoPresenter::run()
{
    while (true) {
        double now = monotonicNow();
        double wait = present(now);
        if (m_stats.intervalElapsed(now, statsIntervalSeconds)) {
            if (m_stats.due())
                ffmpegMediaLog("%s%s", m_stats.line().c_str(), m_logTag);
            m_stats.reset(now);
        }
        Locker locker { m_lock };
        if (m_stopping)
            return;
        if (!m_woken)
            m_condition.waitFor(m_lock, Seconds(wait));
        m_woken = false;
        if (m_stopping)
            return;
    }
}

double FFmpegVideoPresenter::present(double now)
{
    unsigned events = 0;
    double wait;
    {
        Locker locker { m_lock };
        if (m_stopping)
            return 0;
        unsigned serial = m_serial;
        bool seeking = m_seeking;
        double seekTime = m_seekTime;
        bool running = m_clock->running();
        double rate = m_clock->rate();
        auto position = [&](double at) {
            return seeking ? seekTime : m_clock->time(at);
        };
        double clock = position(now);
        m_decoder->setClockHint(clock);
        bool needFirstFrame = !m_current || m_current->serial != serial;
        std::optional<double> untilDue;
        while (RefPtr frame = m_decoder->peekFrame(serial)) {
            RefPtr next = m_decoder->peekFrame(serial, 1);
            std::optional<double> nextPts;
            if (next)
                nextPts = next->pts;
            // Paused or seeking: the first frame at the new position shows at
            // once, even though the clock has not reached it.
            bool showAtOnce = needFirstFrame && (seeking || !running);
            auto decision = FFmpegMedia::decidePresentation(frame->pts, frame->duration, nextPts, clock, showAtOnce);
            if (decision.action == FFmpegMedia::PresentAction::Wait) {
                untilDue = decision.untilDue;
                break;
            }
            m_decoder->popFrame();
            if (running && !seeking)
                m_stats.record(decision);
            if (decision.action == FFmpegMedia::PresentAction::Drop) {
                ++m_dropped;
                continue;
            }
            m_current = frame;
            if (m_overlay)
                m_overlay->submit(*frame);
            events |= FrameShown;
            if (needFirstFrame)
                events |= FirstFrame;
            if (needFirstFrame && !running) {
                needFirstFrame = false;
                break;
            }
            needFirstFrame = false;
            // The next frame is judged against a fresh clock reading.
            clock = position(monotonicNow());
        }
        if (running && m_decoder->reachedEnd(serial) && !(m_endPosted && m_endSerialPosted == serial)) {
            m_endPosted = true;
            m_endSerialPosted = serial;
            events |= ReachedEnd;
        }
        // Still waiting for the first frame of a seek: keep polling.
        wait = FFmpegMedia::presenterWaitSeconds(untilDue, rate, running || needFirstFrame);
    }
    if (events)
        post(events);
    return wait;
}

void FFmpegVideoPresenter::post(unsigned events)
{
    if (m_pendingEvents.fetch_or(events))
        return;
    callOnMainThread([protectedThis = Ref { *this }] {
        protectedThis->deliver();
    });
}

void FFmpegVideoPresenter::deliver()
{
    unsigned events = m_pendingEvents.exchange(0);
    if (events && m_handler)
        m_handler(events);
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
