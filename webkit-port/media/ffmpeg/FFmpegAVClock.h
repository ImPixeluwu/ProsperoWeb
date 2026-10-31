// SPDX-License-Identifier: GPL-3.0-or-later
//
// Playback clock of the ffmpeg media engine. Engine-independent: callers
// pass the current monotonic time in seconds, and (when audio is the master)
// the time of the audio sample currently leaving the speakers. Video frames
// are scheduled against time(); the same clock can drive an MSE player later.
//
// Rules:
//  - audio master: while audio is playing at rate 1, time() follows the audio
//    position (base pts + frames consumed / sample rate - output latency), so
//    an audio underrun stalls video as well;
//  - otherwise the clock extrapolates from the last anchor with the playback
//    rate, so silent media, audio-less video, rate != 1 and the tail of a
//    video whose audio ended early still advance;
//  - time() never goes backwards except across reset() (a seek).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace WebCore {
namespace FFmpegMedia {

// Media time of the sample being heard: base pts of the first sample written
// after the last flush, plus the frames the sink has consumed since, minus
// the sink latency. Never earlier than basePts.
inline double audioClockTime(double basePts, int64_t framesConsumed, int sampleRate, double latencySeconds)
{
    if (sampleRate <= 0)
        return basePts;
    double t = basePts + static_cast<double>(framesConsumed) / sampleRate - latencySeconds;
    return std::max(basePts, t);
}

class AVClock {
public:
    void reset(double mediaTime, double now)
    {
        m_anchorMedia = mediaTime;
        m_anchorWall = now;
        m_last = mediaTime;
    }

    void setRunning(bool running, double now)
    {
        if (running == m_running)
            return;
        reanchor(now);
        m_running = running;
    }

    // Buffering: while stalled, the extrapolated clock holds still.
    void setStalled(bool stalled, double now)
    {
        if (stalled == m_stalled)
            return;
        reanchor(now);
        m_stalled = stalled;
    }

    void setRate(double rate, double now)
    {
        if (!(rate >= 0) || rate == m_rate)
            return;
        reanchor(now);
        m_rate = rate;
    }

    double rate() const { return m_rate; }
    bool running() const { return m_running; }
    bool stalled() const { return m_stalled; }

    // Audio may only drive the clock at normal speed: there is no
    // time-stretching, the engine mutes audio at other rates.
    bool audioCanBeMaster() const { return m_rate == 1.0; }

    double time(double now, std::optional<double> audioTime = std::nullopt)
    {
        double t;
        if (audioTime && audioCanBeMaster() && m_running) {
            // Anchor on the value actually returned, so a later fallback to
            // extrapolation continues from it.
            t = std::max(*audioTime, m_last);
            m_anchorMedia = t;
            m_anchorWall = now;
        } else
            t = std::max(extrapolate(now), m_last);
        m_last = t;
        return t;
    }

private:
    double extrapolate(double now) const
    {
        if (!m_running || m_stalled)
            return m_anchorMedia;
        return m_anchorMedia + std::max(0.0, now - m_anchorWall) * m_rate;
    }

    void reanchor(double now)
    {
        m_anchorMedia = std::max(extrapolate(now), m_last);
        m_anchorWall = now;
    }

    double m_anchorMedia { 0 };
    double m_anchorWall { 0 };
    double m_last { 0 };
    double m_rate { 1 };
    bool m_running { false };
    bool m_stalled { false };
};

// Presentation decision for one decoded video frame at clock time `now`.
enum class FrameDecision { Wait, Show, Drop };

// A frame is shown once its pts is reached; it is dropped instead when the
// next frame is also due (the display would only flash it).
inline FrameDecision decideFrame(double framePts, std::optional<double> nextFramePts, double clock)
{
    constexpr double earlyTolerance = 0.005;
    if (framePts > clock + earlyTolerance)
        return FrameDecision::Wait;
    if (nextFramePts && *nextFramePts <= clock + earlyTolerance)
        return FrameDecision::Drop;
    return FrameDecision::Show;
}

} // namespace FFmpegMedia
} // namespace WebCore
