// SPDX-License-Identifier: GPL-3.0-or-later
//
// Byte-stream input for libavformat backed by WebCore's media resource
// loader (PlatformMediaResourceLoader), so media requests carry the page's
// cookies, referrer, CORS mode and CSP checks and share the network process
// with the rest of the page. The demuxer installs it as the top-level
// AVIOContext and as AVFormatContext::io_open, which libavformat's HLS
// demuxer uses for playlists, keys and segments.
//
// Threading: read()/seek() run on the demuxer thread and block (polling an
// abort flag every 50 ms); requests are started on the main thread and the
// PlatformMediaResourceClient callbacks arrive there. The main thread never
// waits for the demuxer thread, so the two cannot deadlock.
//
// Stalls and failures: a request that delivers nothing for 8 s while the
// demuxer waits, a network failure or a 5xx/408/429 response is cancelled
// and issued again from the current offset (Range), after 0.5, 1 and 2 s;
// then the stream fails (NetworkRetry in FFmpegPlaybackPolicy.h). The
// waits of the demuxer thread drive the stall check and the backoff.

#pragma once

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "FFmpegPlaybackPolicy.h"
#include <atomic>
#include <span>
#include <string>
#include <wtf/Condition.h>
#include <wtf/Deque.h>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/URL.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

struct AVIOContext;

namespace WebCore {

class PlatformMediaResource;
class PlatformMediaResourceLoader;
class ResourceResponse;

// State shared by every stream one demuxer opens.
class FFmpegIOShared : public ThreadSafeRefCounted<FFmpegIOShared> {
public:
    static Ref<FFmpegIOShared> create() { return adoptRef(*new FFmpegIOShared); }

    std::atomic<bool> aborted { false };
    std::atomic<bool> networkError { false };
    std::atomic<bool> didPassAccessControlCheck { true };
    std::atomic<bool> sawResponse { false };
    std::atomic<uint64_t> bytesReceived { 0 };
    // A seek is waiting for the demuxer thread: a read of the top-level
    // stream that would block returns AVERROR(EINTR) instead, so the seek
    // does not wait for data at the old position.
    std::atomic<bool> seekPending { false };
    // libavformat's HLS demuxer asked for an skd:// key (FairPlay DRM).
    std::atomic<bool> fairPlayKeyRequested { false };
    std::atomic<unsigned> requestCount { 0 };

private:
    FFmpegIOShared() = default;
};

class FFmpegResourceStream final : public ThreadSafeRefCounted<FFmpegResourceStream> {
public:
    static Ref<FFmpegResourceStream> create(Ref<PlatformMediaResourceLoader>&&, const URL&, Ref<FFmpegIOShared>&&);
    ~FFmpegResourceStream();

    // Before open(): serve only bytes [offset, endOffset) of the resource
    // (HLS EXT-X-BYTERANGE, passed by libavformat as the "offset" and
    // "end_offset" options; endOffset 0 means to the end). Reads then start
    // at `offset`.
    void setByteRange(uint64_t offset, uint64_t endOffset);
    // The top-level stream gives way to a pending seek (see seekPending).
    void setInterruptibleBySeek(bool interruptible) { m_interruptibleBySeek = interruptible; }

    // Demuxer thread. open() issues the first request and waits for the
    // response; it returns 0 or a negative AVERROR.
    int open();
    int read(uint8_t*, int size);
    int64_t seek(int64_t offset, int whence);
    void close();

    // Creates an AVIOContext that owns a reference to `stream`. Returns
    // nullptr on allocation failure.
    static AVIOContext* createAVIOContext(Ref<FFmpegResourceStream>&&);
    static bool isOurAVIOContext(const AVIOContext*);
    // Closes the stream and frees the context and its buffer.
    static void destroyAVIOContext(AVIOContext*);

    // Main thread, from the resource client.
    void didReceiveResponse(unsigned requestId, const ResourceResponse&, bool passedAccessControlCheck, bool& shouldContinue);
    void didReceiveData(unsigned requestId, std::span<const uint8_t>);
    void didFinish(unsigned requestId);
    void didFail(unsigned requestId, bool isAccessControlFailure);

private:
    FFmpegResourceStream(Ref<PlatformMediaResourceLoader>&&, const URL&, Ref<FFmpegIOShared>&&);

    enum class RequestState : uint8_t { None, Pending, Receiving, Finished, Failed, Suspended, RetryWait };

    struct Chunk {
        uint64_t position { 0 };
        Vector<uint8_t> data;
        uint64_t end() const { return position + data.size(); }
    };

    void dispatchRequest(unsigned requestId, uint64_t position, uint64_t rangeEnd);
    void startRequestLocked(uint64_t position, bool keepBuffer) WTF_REQUIRES_LOCK(m_lock);
    void suspendLocked() WTF_REQUIRES_LOCK(m_lock);
    void stopResourceLocked() WTF_REQUIRES_LOCK(m_lock);
    uint64_t bufferStartLocked() const WTF_REQUIRES_LOCK(m_lock);
    uint64_t bufferEndLocked() const WTF_REQUIRES_LOCK(m_lock);
    size_t copyOutLocked(uint8_t*, size_t) WTF_REQUIRES_LOCK(m_lock);
    void trimHistoryLocked() WTF_REQUIRES_LOCK(m_lock);
    bool waitLocked() WTF_REQUIRES_LOCK(m_lock);
    // Stall detection and the retry backoff (demuxer thread, from the waits).
    void checkRequestLocked() WTF_REQUIRES_LOCK(m_lock);
    // A stall or a retryable failure: schedules the next retry or fails.
    void retryOrFailLocked(const char* reason) WTF_REQUIRES_LOCK(m_lock);
    void restartRequestLocked() WTF_REQUIRES_LOCK(m_lock);
    std::string hostForLog() const;

    const Ref<PlatformMediaResourceLoader> m_loader;
    const URL m_url;
    const Ref<FFmpegIOShared> m_shared;

    Lock m_lock;
    Condition m_condition;
    Deque<Chunk> m_chunks WTF_GUARDED_BY_LOCK(m_lock);
    uint64_t m_readPosition WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    // Bytes of a 200 response to drop because the server ignored Range.
    uint64_t m_skipBytes WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    uint64_t m_requestPosition WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    uint64_t m_receivedInRequest WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    int64_t m_size WTF_GUARDED_BY_LOCK(m_lock) { -1 };
    unsigned m_requestId WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    RequestState m_state WTF_GUARDED_BY_LOCK(m_lock) { RequestState::None };
    bool m_rangesSupported WTF_GUARDED_BY_LOCK(m_lock) { true };
    bool m_closed WTF_GUARDED_BY_LOCK(m_lock) { false };
    // Exclusive end of the first request's Range (0: open-ended).
    uint64_t m_rangeEnd WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    // The byte range of setByteRange(), for retries of the first request.
    uint64_t m_byteRangeEnd WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    // Monotonic seconds of the last request start, response or data.
    double m_lastProgressAt WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    double m_retryAt WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    FFmpegMedia::NetworkRetry m_retry WTF_GUARDED_BY_LOCK(m_lock);
    bool m_interruptibleBySeek { false };
    RefPtr<PlatformMediaResource> m_resource WTF_GUARDED_BY_LOCK(m_lock);
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
