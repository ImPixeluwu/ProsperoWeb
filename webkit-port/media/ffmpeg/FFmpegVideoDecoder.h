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
//
// Remote mode (PROSPEROWEB_VIDEO_DECODE=ui, the default on PlayStation):
// given the player's video overlay, the decoder first asks the UI process
// to decode the stream in hardware (FFmpegVideoOverlayClient.h,
// RemoteVideoStream). If the UI accepts, the worker thread sends it the
// packets instead of running libavcodec, and every frame the UI reports
// becomes a pixel-less FFmpegVideoFrame (pts, duration, size) in the same
// output queue, so the presenter and the A/V clock work unchanged; showing
// one names it to the UI by its pts. If the UI refuses (codec, profile,
// no decoder) or fails later, libavcodec is opened and decoding continues
// here from the next keyframe.

#pragma once

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "FFmpegPlaybackPolicy.h"
#include "FFmpegUtilities.h"
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>

struct SwsContext;

namespace WebCore {

class FFmpegPacketQueue;
class FFmpegVideoOverlay;

namespace FFmpegMedia {
namespace VideoWire {
class RemoteVideoStream;
struct StreamOpen;
}
}

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

    // A frame the UI process decoded and keeps (remote decoding): no pixels
    // here, `remotePts` names it to the UI.
    static Ref<FFmpegVideoFrame> createRemote(int width, int height, uint32_t stream, int64_t remotePts)
    {
        Ref frame = adoptRef(*new FFmpegVideoFrame(width, height, nullptr, 0));
        frame->remoteStream = stream;
        frame->remotePts = remotePts;
        return frame;
    }

    ~FFmpegVideoFrame() { av_free(m_pixels); }

    bool isRemote() const { return !m_pixels; }

    int width() const { return m_width; }
    int height() const { return m_height; }
    size_t stride() const { return FFmpegMedia::videoFrameStride(m_width); }
    uint8_t* data() { return m_pixels; }
    const uint8_t* data() const { return m_pixels; }
    size_t sizeInBytes() const { return m_bytes; }

    double pts { 0 };
    double duration { 0 };
    unsigned serial { 0 };
    uint32_t remoteStream { 0 };
    int64_t remotePts { 0 };

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
    // Returns nullptr when no decoder can be opened for the stream. With an
    // overlay, the UI process is asked to decode it (remote mode, see above).
    static RefPtr<FFmpegVideoDecoder> create(const FFmpegStreamInfo&, FFmpegPacketQueue&, FFmpegVideoOverlay* = nullptr);
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

    // Average milliseconds per frame since the previous call (main thread):
    // time in libavcodec per decoded frame, swscale per converted frame.
    struct Timing {
        double decodeMilliseconds { 0 };
        double convertMilliseconds { 0 };
    };
    Timing takeTiming();
    int threadCount() const { return m_remoteActive ? 0 : m_codec->thread_count; }
    // "ui hw" while the UI decodes, "software" otherwise (logs).
    const char* decodeMode() const { return m_remoteActive ? "ui hw" : "software"; }

    unsigned decodedFrames() const { return m_decodedFrames; }
    unsigned droppedFrames() const { return m_droppedFrames; }
    int outputWidth() const { return m_outputWidth; }
    int outputHeight() const { return m_outputHeight; }

private:
    FFmpegVideoDecoder(const FFmpegStreamInfo&, FFmpegPacketQueue&, AVCodecContextPtr&&, bool codecOpen);

    void run();
    // libavcodec, opened late when remote decoding was tried first.
    bool openSoftwareDecoder();
    // Remote mode: returns when stopping, or after the UI refused or failed
    // (libavcodec is open then and took the packet in hand).
    void runRemote();
    void processRemoteNotices();
    void sendRemotePacket(AVPacket*);
    void emitRemoteFrame(int64_t pts, int width, int height);
    void endRemote(const char* why);
    void flushForSerial(unsigned serial);
    bool handlePacket(AVPacket*);
    void receiveFrames();
    void emitFrame(AVFrame*);
    RefPtr<FFmpegVideoFrame> takePoolFrame(int width, int height);
    void updateOutputSize(int width, int height);

    const AVRational m_timeBase;
    std::shared_ptr<FFmpegMedia::VideoWire::RemoteVideoStream> m_remote;
    std::unique_ptr<FFmpegMedia::VideoWire::StreamOpen> m_remoteOpen;
    std::atomic<bool> m_remoteActive { false };
    bool m_codecOpen { false };
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
    std::atomic<unsigned> m_convertedFrames { 0 };
    std::atomic<int64_t> m_decodeMicros { 0 };
    std::atomic<int64_t> m_convertMicros { 0 };
    // takeTiming() snapshot (main thread).
    unsigned m_timingDecoded { 0 };
    unsigned m_timingConverted { 0 };
    int64_t m_timingDecodeMicros { 0 };
    int64_t m_timingConvertMicros { 0 };
    std::atomic<int> m_outputWidth { 0 };
    std::atomic<int> m_outputHeight { 0 };

    // Decoder thread only.
    unsigned m_serial { 0 };
    bool m_waitingForKeyframe { false };
    bool m_skippingLoopFilter { false };
    unsigned m_lateFrames { 0 };
    int64_t m_pendingConvertMicros { 0 };
    unsigned m_sendErrors { 0 };
    Vector<Ref<FFmpegVideoFrame>> m_pool;
    // Remote mode: packet durations by pts (the UI reports pts only).
    std::map<int64_t, int64_t> m_remoteDurations;

    mutable Lock m_lock;
    Condition m_condition;
    Deque<Ref<FFmpegVideoFrame>> m_output WTF_GUARDED_BY_LOCK(m_lock);
    unsigned m_endSerial WTF_GUARDED_BY_LOCK(m_lock) { std::numeric_limits<unsigned>::max() };
    double m_seekTarget WTF_GUARDED_BY_LOCK(m_lock) { -1 };
    unsigned m_seekSerial WTF_GUARDED_BY_LOCK(m_lock) { 0 };
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
