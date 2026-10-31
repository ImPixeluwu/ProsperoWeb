// SPDX-License-Identifier: GPL-3.0-or-later
//
// Software video decoding for the ffmpeg media engine: a worker thread pulls
// packets from the demuxer queue, decodes them with libavcodec (frame and
// slice threads) and converts each picture with libswscale into an opaque
// BGRA FFmpegVideoFrame taken from a small recycling pool. Decoded frames
// wait in a short output queue until the player's presentation tick shows
// them against the AVClock.
//
// The decoder only knows packets and frames, not WebCore players, so an MSE
// SourceBuffer-fed player can reuse it with its own packet queue.

#pragma once

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "FFmpegPlaybackPolicy.h"
#include "FFmpegUtilities.h"
#include <atomic>
#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>

struct SwsContext;

namespace WebCore {

class FFmpegPacketQueue;

// Pixels live in an av_malloc() block (64-byte aligned) with rows padded to
// videoFrameStride(): libswscale's SIMD stores run past the row end (see
// FFmpegPlaybackPolicy.h), which overran a tightly packed buffer into the
// heap allocator's metadata.
class FFmpegVideoFrame final : public ThreadSafeRefCounted<FFmpegVideoFrame> {
public:
    // nullptr when the allocation fails.
    static RefPtr<FFmpegVideoFrame> create(int width, int height)
    {
        size_t bytes = FFmpegMedia::videoFrameBytes(width, height);
        if (!bytes)
            return nullptr;
        auto* pixels = static_cast<uint8_t*>(av_malloc(bytes));
        if (!pixels)
            return nullptr;
        return adoptRef(*new FFmpegVideoFrame(width, height, pixels, bytes));
    }

    ~FFmpegVideoFrame() { av_free(m_pixels); }

    int width() const { return m_width; }
    int height() const { return m_height; }
    size_t stride() const { return FFmpegMedia::videoFrameStride(m_width); }
    uint8_t* data() { return m_pixels; }
    const uint8_t* data() const { return m_pixels; }
    size_t sizeInBytes() const { return m_bytes; }

    double pts { 0 };
    double duration { 0 };
    unsigned serial { 0 };

private:
    FFmpegVideoFrame(int width, int height, uint8_t* pixels, size_t bytes)
        : m_width(width)
        , m_height(height)
        , m_pixels(pixels)
        , m_bytes(bytes)
    {
    }

    const int m_width;
    const int m_height;
    uint8_t* const m_pixels;
    const size_t m_bytes;
};

class FFmpegVideoDecoder final : public ThreadSafeRefCounted<FFmpegVideoDecoder> {
public:
    // Returns nullptr when no decoder can be opened for the stream.
    static RefPtr<FFmpegVideoDecoder> create(const FFmpegStreamInfo&, FFmpegPacketQueue&);
    ~FFmpegVideoDecoder();

    void start();
    void stop();

    // Output queue (main thread). Frames of an older serial are dropped
    // internally; peek/pop only ever return frames of `serial`.
    RefPtr<FFmpegVideoFrame> peekFrame(unsigned serial, size_t index = 0);
    void popFrame();
    size_t queuedFrames() const;
    // The decoder drained the stream for `serial` and every frame was popped.
    bool reachedEnd(unsigned serial) const;
    bool hasFatalError() const { return m_fatalError; }

    // Frames before `time` are decoded but not converted or queued (accurate
    // seek). Call right after the demuxer seek with the same serial.
    void setSeekTarget(double time, unsigned serial);
    // Hidden page: packets are dropped until the next keyframe after resume.
    void setSuspended(bool);
    // Current playback position, used to drop late frames before conversion.
    void setClockHint(double time) { m_clockHint = time; }

    unsigned decodedFrames() const { return m_decodedFrames; }
    unsigned droppedFrames() const { return m_droppedFrames; }
    int outputWidth() const { return m_outputWidth; }
    int outputHeight() const { return m_outputHeight; }

private:
    FFmpegVideoDecoder(const FFmpegStreamInfo&, FFmpegPacketQueue&, AVCodecContextPtr&&);

    void run();
    void flushForSerial(unsigned serial);
    bool handlePacket(AVPacket*);
    void receiveFrames();
    void emitFrame(AVFrame*);
    RefPtr<FFmpegVideoFrame> takePoolFrame(int width, int height);
    void updateOutputSize(int width, int height);

    const AVRational m_timeBase;
    const double m_startTime;
    FFmpegPacketQueue& m_queue;
    AVCodecContextPtr m_codec;
    AVFramePtr m_frame;
    SwsContext* m_sws { nullptr };
    RefPtr<Thread> m_thread;
    std::atomic<bool> m_stopping { false };
    std::atomic<bool> m_fatalError { false };
    std::atomic<bool> m_suspended { false };
    std::atomic<double> m_clockHint { 0 };
    std::atomic<unsigned> m_decodedFrames { 0 };
    std::atomic<unsigned> m_droppedFrames { 0 };
    std::atomic<int> m_outputWidth { 0 };
    std::atomic<int> m_outputHeight { 0 };

    // Decoder thread only.
    unsigned m_serial { 0 };
    bool m_waitingForKeyframe { false };
    bool m_skippingLoopFilter { false };
    unsigned m_lateFrames { 0 };
    unsigned m_sendErrors { 0 };
    Vector<Ref<FFmpegVideoFrame>> m_pool;

    mutable Lock m_lock;
    Condition m_condition;
    Deque<Ref<FFmpegVideoFrame>> m_output WTF_GUARDED_BY_LOCK(m_lock);
    unsigned m_endSerial WTF_GUARDED_BY_LOCK(m_lock) { std::numeric_limits<unsigned>::max() };
    double m_seekTarget WTF_GUARDED_BY_LOCK(m_lock) { -1 };
    unsigned m_seekSerial WTF_GUARDED_BY_LOCK(m_lock) { 0 };
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
