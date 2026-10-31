// SPDX-License-Identifier: GPL-3.0-or-later
//
// Demuxer of the ffmpeg media engine: one thread that opens the resource
// with libavformat (progressive MP4/WebM/MP3/Ogg/..., and HLS through
// libavformat's hls demuxer), selects one video and one audio stream and
// fills a packet queue per stream. Seeks are requested asynchronously and
// tagged with a serial number that flows through the queues to the decoders,
// so stale packets and frames are recognised and dropped.

#pragma once

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "FFmpegPlaybackPolicy.h"
#include "FFmpegResourceIO.h"
#include "FFmpegTimeRanges.h"
#include "FFmpegUtilities.h"
#include <atomic>
#include <optional>
#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/Lock.h>
#include <wtf/Seconds.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Threading.h>
#include <wtf/URL.h>

namespace WebCore {

class PlatformMediaResourceLoader;

class FFmpegPacketQueue {
    WTF_MAKE_NONCOPYABLE(FFmpegPacketQueue);
public:
    FFmpegPacketQueue() = default;

    struct Entry {
        AVPacketPtr packet; // null: end of stream for `serial`
        unsigned serial { 0 };
        bool isEndOfStream() const { return !packet; }
    };

    void setTimeBase(AVRational timeBase) { m_timeBase = timeBase; }
    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool enabled() const { return m_enabled; }

    void push(AVPacketPtr&&, unsigned serial);
    void pushEndOfStream(unsigned serial);
    // Waits up to `timeout`. Returns nullopt on timeout or abort.
    std::optional<Entry> pop(Seconds timeout);
    void flush(unsigned newSerial);
    void abort();

    bool isAborted() const { return m_aborted; }
    unsigned serial() const { return m_serial; }
    FFmpegMedia::QueueLevel level() const;
    size_t bytes() const;

private:
    mutable Lock m_lock;
    Condition m_condition;
    Deque<Entry> m_entries WTF_GUARDED_BY_LOCK(m_lock);
    size_t m_bytes WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    int64_t m_lastTimestamp WTF_GUARDED_BY_LOCK(m_lock) { AV_NOPTS_VALUE };
    AVRational m_timeBase { 1, 1 };
    std::atomic<unsigned> m_serial { 0 };
    std::atomic<bool> m_aborted { false };
    bool m_enabled { false };
};

class FFmpegDemuxer final : public ThreadSafeRefCounted<FFmpegDemuxer> {
public:
    static Ref<FFmpegDemuxer> create(const URL&, Ref<PlatformMediaResourceLoader>&&, const String& userAgent);
    ~FFmpegDemuxer();

    void start();
    // Aborts all I/O and joins the thread. Idempotent; main thread.
    void stop();

    enum class State : uint8_t { Opening, Ready, Failed };
    struct Status {
        State state { State::Opening };
        FFmpegMedia::LoadFailure failure { FFmpegMedia::LoadFailure::None };
        bool endOfStream { false };
        bool readerWaiting { false };
        uint64_t bytesReceived { 0 };
        FFmpegMedia::TimeRangeSet buffered;
    };
    Status status() const;

    // Valid once status().state == Ready; immutable from then on.
    const FFmpegStreamInfo* videoStream() const { return m_video.index >= 0 ? &m_video : nullptr; }
    const FFmpegStreamInfo* audioStream() const { return m_audio.index >= 0 ? &m_audio : nullptr; }
    // NaN while unknown, +infinity for live streams.
    double duration() const { return m_duration; }
    bool isLive() const { return m_isLive; }
    bool isSeekable() const { return m_isSeekable; }
    bool isHLS() const { return m_isHLS; }
    bool didPassAccessControlCheck() const { return m_shared->sawResponse && m_shared->didPassAccessControlCheck; }

    FFmpegPacketQueue& videoQueue() { return m_videoQueue; }
    FFmpegPacketQueue& audioQueue() { return m_audioQueue; }

    // Main thread. Packets demuxed after the seek carry `serial`.
    void seek(double time, unsigned serial);

private:
    FFmpegDemuxer(const URL&, Ref<PlatformMediaResourceLoader>&&, const String& userAgent);

    void run();
    bool openInput();
    void selectStreams();
    void readLoop();
    void performSeek(double time, unsigned serial);
    void fail(FFmpegMedia::LoadFailure);
    void closeInput();
    bool usesResourceLoader(const URL&) const;

    static int interruptCallback(void*);
    static int ioOpen(AVFormatContext*, AVIOContext**, const char* url, int flags, AVDictionary** options);
    static int ioClose(AVFormatContext*, AVIOContext*);

    const URL m_url;
    const Ref<PlatformMediaResourceLoader> m_loader;
    const String m_userAgent;
    const Ref<FFmpegIOShared> m_shared;
    RefPtr<Thread> m_thread;

    // Demuxer thread only.
    AVFormatContext* m_context { nullptr };
    AVIOContext* m_topLevelIO { nullptr };
    int (*m_defaultIoOpen)(AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**) { nullptr };
    int (*m_defaultIoClose)(AVFormatContext*, AVIOContext*) { nullptr };
    unsigned m_serial { 0 };
    double m_segmentStart { 0 };

    // Written by the demuxer thread before State::Ready is published.
    FFmpegStreamInfo m_video;
    FFmpegStreamInfo m_audio;
    double m_duration { std::numeric_limits<double>::quiet_NaN() };
    bool m_isLive { false };
    bool m_isSeekable { false };
    bool m_isHLS { false };

    FFmpegPacketQueue m_videoQueue;
    FFmpegPacketQueue m_audioQueue;

    mutable Lock m_lock;
    Condition m_condition;
    State m_state WTF_GUARDED_BY_LOCK(m_lock) { State::Opening };
    FFmpegMedia::LoadFailure m_failure WTF_GUARDED_BY_LOCK(m_lock) { FFmpegMedia::LoadFailure::None };
    bool m_endOfStream WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_readerWaiting WTF_GUARDED_BY_LOCK(m_lock) { false };
    FFmpegMedia::TimeRangeSet m_buffered WTF_GUARDED_BY_LOCK(m_lock);
    std::optional<std::pair<double, unsigned>> m_pendingSeek WTF_GUARDED_BY_LOCK(m_lock);
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
