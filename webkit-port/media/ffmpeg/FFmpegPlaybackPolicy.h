// SPDX-License-Identifier: GPL-3.0-or-later
//
// Buffering thresholds and the HTML readyState/networkState decisions of the
// ffmpeg media engine, as pure functions so they can be unit tested and
// reused by a future MSE player.

#pragma once

#include <cstddef>

namespace WebCore {
namespace FFmpegMedia {

// Demuxer read-ahead limits. Reading pauses (networkState IDLE) when either
// the total queued bytes reach maxQueuedBytes or every enabled stream has at
// least maxQueuedSeconds queued; it resumes once that is no longer true.
inline constexpr size_t maxQueuedBytes = 48u << 20;
inline constexpr double maxQueuedSeconds = 15.0;
// A stream with fewer packets than this is never considered "full" (sparse
// streams or broken durations must not stall the other stream).
inline constexpr size_t minQueuedPackets = 25;

struct QueueLevel {
    bool enabled { false };
    size_t packets { 0 };
    double seconds { 0 };
};

inline bool streamHasEnough(const QueueLevel& queue)
{
    return !queue.enabled || (queue.packets >= minQueuedPackets && queue.seconds >= maxQueuedSeconds);
}

inline bool demuxerShouldWait(size_t totalBytes, const QueueLevel& video, const QueueLevel& audio)
{
    if (totalBytes >= maxQueuedBytes)
        return true;
    if (!video.enabled && !audio.enabled)
        return false;
    return streamHasEnough(video) && streamHasEnough(audio);
}

// Thresholds on contiguous data ahead of the current position.
inline constexpr double futureDataSeconds = 0.3;
inline constexpr double enoughDataSeconds = 3.0;

// Mirrors MediaPlayer::ReadyState without depending on WebCore.
enum class ReadyLevel { HaveNothing, HaveMetadata, HaveCurrentData, HaveFutureData, HaveEnoughData };

struct ReadyInputs {
    bool metadataLoaded { false };
    bool seeking { false };
    // A frame for the current position is decoded (or, for audio-only media,
    // decoded audio is queued for it).
    bool currentDataAvailable { false };
    double bufferedAheadSeconds { 0 };
    // The demuxer has delivered everything (end of stream).
    bool endOfStream { false };
};

inline ReadyLevel computeReadyLevel(const ReadyInputs& in)
{
    if (!in.metadataLoaded)
        return ReadyLevel::HaveNothing;
    if (in.seeking || !in.currentDataAvailable)
        return ReadyLevel::HaveMetadata;
    if (in.endOfStream || in.bufferedAheadSeconds >= enoughDataSeconds)
        return ReadyLevel::HaveEnoughData;
    if (in.bufferedAheadSeconds >= futureDataSeconds)
        return ReadyLevel::HaveFutureData;
    return ReadyLevel::HaveCurrentData;
}

// Mirrors MediaPlayer::NetworkState.
enum class NetworkLevel { Empty, Idle, Loading, Loaded, FormatError, NetworkError, DecodeError };

enum class LoadFailure { None, Open, Network, Decode };

struct NetworkInputs {
    bool started { false };
    bool metadataLoaded { false };
    LoadFailure failure { LoadFailure::None };
    bool endOfStream { false };
    bool readerWaiting { false };
};

inline NetworkLevel computeNetworkLevel(const NetworkInputs& in)
{
    switch (in.failure) {
    case LoadFailure::Open:
        // Nothing usable was found: the resource is not a playable format
        // (or could not be fetched at all, which HTML reports the same way
        // before metadata).
        return in.metadataLoaded ? NetworkLevel::NetworkError : NetworkLevel::FormatError;
    case LoadFailure::Network:
        return in.metadataLoaded ? NetworkLevel::NetworkError : NetworkLevel::FormatError;
    case LoadFailure::Decode:
        return NetworkLevel::DecodeError;
    case LoadFailure::None:
        break;
    }
    if (!in.started)
        return NetworkLevel::Idle;
    if (in.endOfStream)
        return NetworkLevel::Loaded;
    if (in.readerWaiting && in.metadataLoaded)
        return NetworkLevel::Idle;
    return NetworkLevel::Loading;
}

} // namespace FFmpegMedia
} // namespace WebCore
