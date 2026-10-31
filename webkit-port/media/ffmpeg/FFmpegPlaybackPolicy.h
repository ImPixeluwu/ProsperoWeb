// SPDX-License-Identifier: GPL-3.0-or-later
//
// Buffering thresholds and the HTML readyState/networkState decisions of the
// ffmpeg media engine, as pure functions so they can be unit tested and
// reused by a future MSE player.

#pragma once

#include <cstddef>
#include <cstdint>

namespace WebCore {
namespace FFmpegMedia {

// Demuxer read-ahead limits. Reading pauses (networkState IDLE) when either
// the total queued bytes reach maxQueuedBytes or every enabled stream has at
// least maxQueuedSeconds queued; it resumes once that is no longer true.
inline constexpr size_t maxQueuedBytes = 24u << 20;
inline constexpr double maxQueuedSeconds = 10.0;
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

// Network read-ahead per stream: a request is suspended above the high water
// mark of unread data and resumed below the low one. A server that supports
// Range resumes with a Range request. One that does not resumes by
// restarting the download and skipping to the position, which is expensive,
// so it gets a higher (but still finite) cap: buffering it without limit
// could take the whole body of a long progressive video into memory.
inline constexpr uint64_t networkHighWaterBytes = 8u << 20;
inline constexpr uint64_t networkLowWaterBytes = 2u << 20;
inline constexpr uint64_t networkNoRangeHighWaterBytes = 32u << 20;

// unreadBytes: received data at or after the read position. size: total
// resource size, or < 0 when unknown (no Content-Length, chunked).
// receivedEnd: offset one past the last received byte.
inline bool networkShouldSuspend(uint64_t unreadBytes, uint64_t receivedEnd, int64_t size, bool rangesSupported)
{
    if (size > 0 && receivedEnd >= static_cast<uint64_t>(size))
        return false; // everything is here; the request is about to finish
    return unreadBytes > (rangesSupported ? networkHighWaterBytes : networkNoRangeHighWaterBytes);
}

inline bool networkShouldResume(uint64_t unreadBytes)
{
    return unreadBytes < networkLowWaterBytes;
}

// Layout of a decoded BGRA video frame. libswscale's x86 output loops store
// 8 pixels per iteration (REAL_WRITEBGR32 in libswscale/x86/
// swscale_template.c: "add $8, index; cmp dstw, index; jb"), so a row is
// written up to the next multiple of 8 pixels: a tightly packed buffer
// (stride = width * 4) is overrun by up to 28 bytes after its last row,
// into the allocator's metadata. Rows are therefore padded to 64 bytes
// (which is also what libswscale's SIMD paths want for alignment) and the
// buffer gets a spare tail.
inline constexpr size_t videoFrameAlignment = 64;
inline constexpr size_t videoFrameTailPadding = 256;

inline size_t videoFrameStride(int width)
{
    if (width <= 0)
        return 0;
    size_t bytes = static_cast<size_t>(width) * 4;
    return (bytes + videoFrameAlignment - 1) & ~(videoFrameAlignment - 1);
}

inline size_t videoFrameBytes(int width, int height)
{
    if (width <= 0 || height <= 0)
        return 0;
    return videoFrameStride(width) * static_cast<size_t>(height) + videoFrameTailPadding;
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
