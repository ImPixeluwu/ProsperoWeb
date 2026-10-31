// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audio decoding for the ffmpeg media engine: a worker thread decodes
// packets, resamples them with libswresample to float32 stereo at the sink
// rate and writes them into a ring buffer that the AudioSink drains. The
// timeline stays linear: gaps in the source timestamps are filled with
// silence, so "base pts + frames consumed / rate" is the audio clock.
//
// Like the video decoder it only depends on a packet queue, so an MSE player
// can feed it the same way.

#pragma once

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "FFmpegRingBuffer.h"
#include "FFmpegUtilities.h"
#include <atomic>
#include <optional>
#include <span>
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Threading.h>
#include <wtf/Vector.h>

struct SwrContext;

namespace WebCore {

class FFmpegPacketQueue;

class FFmpegAudioDecoder final : public ThreadSafeRefCounted<FFmpegAudioDecoder> {
public:
    static RefPtr<FFmpegAudioDecoder> create(const FFmpegStreamInfo&, FFmpegPacketQueue&, int outputRate, int outputChannels);
    ~FFmpegAudioDecoder();

    void start();
    void stop();

    // Sink thread: fills `out` (interleaved), silence where no data.
    void render(std::span<float> out);

    // Media time of the sample being heard, or nullopt before the first
    // sample of the current serial is available.
    std::optional<double> clockTime(double sinkLatency) const;
    // Seconds of decoded audio waiting in the ring buffer.
    double bufferedSeconds() const;
    // End of stream decoded and every sample rendered, for `serial`.
    bool drained(unsigned serial) const;
    // Decoded data for the current position exists (for `serial`).
    bool hasData(unsigned serial) const;
    bool hasFatalError() const { return m_fatalError; }

    void setSeekTarget(double time, unsigned serial);
    // Discard mode (playback rate != 1): no time-stretching, so decoded
    // audio is dropped up to `clockHint` and the sink plays silence; leaving
    // discard mode resynchronises the ring buffer.
    void setDiscard(bool);
    void setClockHint(double time) { m_clockHint = time; }
    void setGain(float gain) { m_gain = gain; }

private:
    FFmpegAudioDecoder(const FFmpegStreamInfo&, FFmpegPacketQueue&, AVCodecContextPtr&&, int outputRate, int outputChannels);

    void run();
    void flushForSerial(unsigned serial);
    void decodePacket(AVPacket*);
    void receiveFrames();
    bool ensureResampler(const AVFrame*);
    void writeFrame(const AVFrame*);
    void writeSamples(std::span<const float>, double pts);
    bool waitForSpace() WTF_REQUIRES_LOCK(m_lock);

    const AVRational m_timeBase;
    const double m_startTime;
    const int m_outputRate;
    const int m_outputChannels;
    FFmpegPacketQueue& m_queue;
    AVCodecContextPtr m_codec;
    AVFramePtr m_frame;
    SwrContext* m_swr { nullptr };
    RefPtr<Thread> m_thread;
    std::atomic<bool> m_stopping { false };
    std::atomic<bool> m_fatalError { false };
    std::atomic<bool> m_discard { false };
    std::atomic<bool> m_needsResync { false };
    std::atomic<double> m_clockHint { 0 };
    std::atomic<float> m_gain { 1 };

    // Decoder thread only.
    unsigned m_serial { 0 };
    int m_swrInputRate { 0 };
    int m_swrInputFormat { -1 };
    AVChannelLayout m_swrInputLayout { };
    Vector<float> m_converted;

    mutable Lock m_lock;
    Condition m_condition;
    FFmpegMedia::RingBuffer<float> m_ring WTF_GUARDED_BY_LOCK(m_lock);
    unsigned m_ringSerial WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    std::optional<double> m_basePts WTF_GUARDED_BY_LOCK(m_lock);
    int64_t m_framesWritten WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    int64_t m_framesConsumed WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    unsigned m_endSerial WTF_GUARDED_BY_LOCK(m_lock) { std::numeric_limits<unsigned>::max() };
    double m_seekTarget WTF_GUARDED_BY_LOCK(m_lock) { -1 };
    unsigned m_seekSerial WTF_GUARDED_BY_LOCK(m_lock) { 0 };
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
