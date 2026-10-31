// SPDX-License-Identifier: GPL-3.0-or-later
//
// Engine-independent policy of the ffmpeg MSE player (no WebCore, WTF or
// ffmpeg headers; unit tested on the host by tests/media/):
//  - how many samples a SourceBuffer track may push into a decoder queue;
//  - the readyState the player reports, given the one the MediaSource
//    computed from its buffered ranges;
//  - where playback would stall, from buffered ranges.

#pragma once

#include "FFmpegPlaybackPolicy.h"
#include "FFmpegTimeRanges.h"
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace WebCore {
namespace FFmpegMedia {

// Samples already live in the SourceBuffer's sample map, so the decoder
// queues only hold enough to keep the decoder threads busy: a SourceBuffer
// track stops being "ready for more samples" at mseQueueFullSeconds (or
// mseQueueFullPackets) and is asked again once the queue is below
// mseQueueLowSeconds.
inline constexpr double mseQueueFullSeconds = 1.5;
inline constexpr size_t mseQueueFullPackets = 240;
inline constexpr double mseQueueLowSeconds = 0.75;
// A queue with fewer packets is never full: broken durations must not stall
// a track.
inline constexpr size_t mseQueueMinPackets = 8;

inline bool mseQueueAcceptsMore(const QueueLevel& level)
{
    if (level.packets < mseQueueMinPackets)
        return true;
    return level.seconds < mseQueueFullSeconds && level.packets < mseQueueFullPackets;
}

inline bool mseQueueShouldRefill(const QueueLevel& level)
{
    if (level.packets < mseQueueMinPackets)
        return true;
    return level.seconds < mseQueueLowSeconds && level.packets < mseQueueFullPackets;
}

// Per-SourceBuffer quota (SourceBufferPrivate::platformMaximumBufferSize),
// sized for the PS5's memory and software decoding: about 50 s of 1080p
// H.264 at 10 Mbit/s, several minutes of audio. Past it, appendBuffer()
// first evicts and then throws QuotaExceededError, as in Safari.
inline constexpr size_t mseVideoBufferQuota = 64u << 20;
inline constexpr size_t mseAudioBufferQuota = 8u << 20;

inline size_t mseBufferQuota(bool hasVideo)
{
    return hasVideo ? mseVideoBufferQuota : mseAudioBufferQuota;
}

struct MSEReadyInputs {
    // What MediaSource::monitorSourceBuffers() computed (buffered ranges
    // against currentTime).
    ReadyLevel fromMediaSource { ReadyLevel::HaveNothing };
    bool seeking { false };
    // A video track is being decoded and painted (visible page).
    bool videoRendered { false };
    // A frame of the current video serial is on screen.
    bool videoFrameShown { false };
    // Audio-only (or hidden video): decoded audio exists for the position,
    // or the audio track ended.
    bool audioReady { true };
};

// The MediaSource only knows samples exist; the element must not report
// HAVE_CURRENT_DATA before the player can show the frame (the GStreamer MSE
// player holds it back until preroll, likewise). Seeking holds at
// HAVE_METADATA until the seek completes.
inline ReadyLevel mseReadyLevel(const MSEReadyInputs& in)
{
    if (in.fromMediaSource < ReadyLevel::HaveCurrentData)
        return in.fromMediaSource;
    if (in.seeking)
        return ReadyLevel::HaveMetadata;
    if (in.videoRendered ? !in.videoFrameShown : !in.audioReady)
        return ReadyLevel::HaveMetadata;
    return in.fromMediaSource;
}

// HTML readyState from buffered ranges alone, the way the MediaSource
// monitors them (used for the extrapolated clock's stall decision and the
// console log): no metadata -> HaveNothing; currentTime not buffered ->
// HaveMetadata; less than futureSeconds ahead -> HaveCurrentData (unless the
// buffered range reaches the duration or the stream ended); less than
// enoughSeconds -> HaveFutureData; otherwise HaveEnoughData.
inline ReadyLevel readyLevelFromBuffered(const TimeRangeSet& buffered, double currentTime, double duration, bool ended, bool haveMetadata,
    double futureSeconds = futureDataSeconds, double enoughSeconds = enoughDataSeconds)
{
    if (!haveMetadata)
        return ReadyLevel::HaveNothing;
    double ahead = buffered.bufferedAhead(currentTime);
    if (ahead <= 0) {
        // The end of a finished stream counts as having its data.
        if (std::isfinite(duration) && currentTime >= duration && ended)
            return ReadyLevel::HaveEnoughData;
        return ReadyLevel::HaveMetadata;
    }
    bool reachesEnd = std::isfinite(duration) && currentTime + ahead >= duration - 0.001;
    if (ahead >= enoughSeconds || (reachesEnd && ended))
        return ReadyLevel::HaveEnoughData;
    if (ahead >= futureSeconds || reachesEnd)
        return ReadyLevel::HaveFutureData;
    return ReadyLevel::HaveCurrentData;
}

// Playback runs dry: less than `threshold` buffered ahead and the stream is
// not over.
inline bool mseShouldStall(const TimeRangeSet& buffered, double position, double duration, bool ended, double threshold = 0.1)
{
    if (ended)
        return false;
    if (std::isfinite(duration) && position >= duration - 0.001)
        return false;
    return buffered.bufferedAhead(position) < threshold;
}

// MARK: - Seeks

// A seek goes through three phases: waiting for the page to append data at
// the target (MediaSourcePrivate::waitForTarget), waiting for the
// re-enqueued samples to be decoded, done. The page decides what to fetch
// from currentTime (the target while seeking) and buffered.
enum class MSESeekPhase { WaitingForData, WaitingForFrames, Done };

struct MSESeekInputs {
    bool dataAvailable { false }; // waitForTarget resolved and samples re-enqueued
    bool videoReady { true }; // a frame of the new serial shown, or no/hidden/broken video
    bool audioReady { true }; // audio of the new serial decoded, or no/broken audio
    double secondsSinceReenqueue { 0 };
};

inline constexpr double mseSeekFrameTimeout = 3.0;

inline MSESeekPhase mseSeekPhase(const MSESeekInputs& in)
{
    if (!in.dataAvailable)
        return MSESeekPhase::WaitingForData;
    if ((in.videoReady && in.audioReady) || in.secondsSinceReenqueue > mseSeekFrameTimeout)
        return MSESeekPhase::Done;
    return MSESeekPhase::WaitingForFrames;
}

// Console log while diagnosing seeks and stalls: every append of the first
// eight, then every 20th, but all of them for 30 s after a seek and while
// the position is not buffered.
inline bool mseShouldLogAppend(unsigned appendNumber, double secondsSinceSeek, bool positionBuffered)
{
    if (appendNumber <= 8 || !(appendNumber % 20))
        return true;
    if (secondsSinceSeek >= 0 && secondsSinceSeek < 30)
        return true;
    return !positionBuffered;
}

} // namespace FFmpegMedia
} // namespace WebCore
