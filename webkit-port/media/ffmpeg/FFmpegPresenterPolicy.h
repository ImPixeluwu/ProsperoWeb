// SPDX-License-Identifier: GPL-3.0-or-later
//
// Frame presentation rules of the ffmpeg media engine's presenter thread
// (FFmpegVideoPresenter.h). Engine-independent (no WebCore/WTF/ffmpeg):
// callers pass the clock time, so the host tests drive it with a fake clock.
//
//  - a frame is due once the clock reached its pts (5 ms early tolerance);
//    the presenter sleeps until then, at most maxWaitSeconds at a time,
//    because an audio-driven clock can jump;
//  - a due frame is shown even when it is late, unless it is more than
//    1.5 frame intervals late and a newer frame is already decoded: then it
//    is dropped and the newer one is considered instead;
//  - paused or seeking, the first frame of a new position is shown at once.

#pragma once

#include <algorithm>
#include <cstdio>
#include <optional>
#include <string>

namespace WebCore {
namespace FFmpegMedia {

constexpr double presenterEarlyTolerance = 0.005;
constexpr double presenterLateIntervals = 1.5;
constexpr double presenterDefaultInterval = 1.0 / 30;
constexpr double presenterMinWaitSeconds = 0.001;
constexpr double presenterMaxWaitSeconds = 0.010;
// No frame decoded yet while playing: poll the decoder's output this often.
constexpr double presenterPollSeconds = 0.004;
// Paused, nothing to show: the main thread wakes the presenter on changes.
constexpr double presenterIdleSeconds = 0.020;

enum class PresentAction { Wait, Show, Drop };

struct PresentDecision {
    PresentAction action { PresentAction::Wait };
    // Wait: seconds of media time until the frame is due.
    double untilDue { 0 };
    // Show/Drop: how late the frame is (never negative).
    double lateness { 0 };
};

// Frame duration, else the distance to the next frame, else 30 fps; bounded
// so a broken timestamp neither drops everything nor nothing.
inline double presentationInterval(double framePts, double frameDuration, std::optional<double> nextPts)
{
    double interval = frameDuration;
    if (!(interval > 0) && nextPts && *nextPts > framePts)
        interval = *nextPts - framePts;
    if (!(interval > 0))
        interval = presenterDefaultInterval;
    return std::clamp(interval, 1.0 / 240, 0.25);
}

inline PresentDecision decidePresentation(double framePts, double frameDuration, std::optional<double> nextPts, double clock, bool showAtOnce)
{
    PresentDecision decision;
    if (showAtOnce) {
        decision.action = PresentAction::Show;
        decision.lateness = std::max(0.0, clock - framePts);
        return decision;
    }
    if (framePts > clock + presenterEarlyTolerance) {
        decision.action = PresentAction::Wait;
        decision.untilDue = framePts - clock;
        return decision;
    }
    decision.lateness = std::max(0.0, clock - framePts);
    double interval = presentationInterval(framePts, frameDuration, nextPts);
    decision.action = decision.lateness > presenterLateIntervals * interval && nextPts ? PresentAction::Drop : PresentAction::Show;
    return decision;
}

// Resume from a hidden page (FFmpegVideoDecoder::runRemote). While hidden the
// decoder holds its input queue rather than draining it: a drained queue lets
// the source feed forward to the end of the buffered range, so decode resumes
// far ahead of the audio clock and the player (which only reaches those pts
// much later) shows nothing while the UI's slots fill with unshowable frames.
//
// resumeDecodeAheadOfClock models where decode resumes relative to the clock.
// `drainWhileHidden` false (the fix) keeps the resume position at the hold
// point, which the advancing clock has caught up to or passed, so the frame
// is at or behind the clock and plays at once; true (the old behaviour)
// resumes at the buffered end, ahead of the clock, which freezes. Returns the
// seconds the first decoded frame is ahead of the clock (<= 0: behind, shown
// at once and dropped-behind as needed).
inline double resumeDecodeAheadOfClock(bool drainWhileHidden, double holdPts, double bufferedEndPts,
    double clockAtResume, double gopSeconds)
{
    double resumePts = drainWhileHidden ? bufferedEndPts : holdPts;
    // Decode restarts at the first keyframe at or after the resume position.
    double keyframePts = resumePts + gopSeconds;
    return keyframePts - clockAtResume;
}

// The player recovers after a resume only when the first decoded frame is
// within reach of the clock (not stuck far ahead of it).
inline bool resumeConverges(double aheadOfClockSeconds, double reachSeconds)
{
    return aheadOfClockSeconds <= reachSeconds;
}

// Wall-clock seconds to sleep before the next decision. `untilDue` is media
// time; at playback rate r it passes r times faster.
inline double presenterWaitSeconds(std::optional<double> untilDue, double rate, bool running)
{
    if (!running)
        return presenterIdleSeconds;
    if (!untilDue)
        return presenterPollSeconds;
    double wall = rate > 0 ? *untilDue / rate : *untilDue;
    return std::clamp(wall, presenterMinWaitSeconds, presenterMaxWaitSeconds);
}

// Per-interval counters for the "presenter:" log line.
class PresenterStats {
public:
    void record(const PresentDecision& decision)
    {
        if (decision.action == PresentAction::Wait)
            return;
        ++m_due;
        if (decision.action == PresentAction::Show) {
            ++m_shown;
            m_maxLateness = std::max(m_maxLateness, decision.lateness);
        } else
            ++m_lateDropped;
    }

    unsigned due() const { return m_due; }
    unsigned shown() const { return m_shown; }
    unsigned lateDropped() const { return m_lateDropped; }
    double maxLatenessMilliseconds() const { return m_maxLateness * 1000; }

    // True once `interval` seconds passed since the first call (which only
    // starts the interval); the caller then logs line() and calls reset().
    bool intervalElapsed(double now, double interval)
    {
        if (!m_started) {
            m_started = true;
            m_intervalStart = now;
            return false;
        }
        return now - m_intervalStart >= interval;
    }

    std::string line() const
    {
        char text[128];
        snprintf(text, sizeof(text), "presenter: due %u shown %u late-dropped %u max lateness %.1f ms", m_due, m_shown, m_lateDropped, maxLatenessMilliseconds());
        return text;
    }

    void reset(double now)
    {
        m_due = m_shown = m_lateDropped = 0;
        m_maxLateness = 0;
        m_intervalStart = now;
    }

private:
    unsigned m_due { 0 };
    unsigned m_shown { 0 };
    unsigned m_lateDropped { 0 };
    double m_maxLateness { 0 };
    double m_intervalStart { 0 };
    bool m_started { false };
};

} // namespace FFmpegMedia
} // namespace WebCore
