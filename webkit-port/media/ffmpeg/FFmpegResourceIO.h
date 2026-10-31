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

#pragma once

#if ENABLE(VIDEO) && USE(FFMPEG)

#include <atomic>
#include <span>
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

private:
    FFmpegIOShared() = default;
};

class FFmpegResourceStream final : public ThreadSafeRefCounted<FFmpegResourceStream> {
public:
    static Ref<FFmpegResourceStream> create(Ref<PlatformMediaResourceLoader>&&, const URL&, Ref<FFmpegIOShared>&&);
    ~FFmpegResourceStream();

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

    enum class RequestState : uint8_t { None, Pending, Receiving, Finished, Failed, Suspended };

    struct Chunk {
        uint64_t position { 0 };
        Vector<uint8_t> data;
        uint64_t end() const { return position + data.size(); }
    };

    void dispatchRequest(unsigned requestId, uint64_t position);
    void startRequestLocked(uint64_t position, bool keepBuffer) WTF_REQUIRES_LOCK(m_lock);
    void suspendLocked() WTF_REQUIRES_LOCK(m_lock);
    void stopResourceLocked() WTF_REQUIRES_LOCK(m_lock);
    uint64_t bufferStartLocked() const WTF_REQUIRES_LOCK(m_lock);
    uint64_t bufferEndLocked() const WTF_REQUIRES_LOCK(m_lock);
    size_t copyOutLocked(uint8_t*, size_t) WTF_REQUIRES_LOCK(m_lock);
    void trimHistoryLocked() WTF_REQUIRES_LOCK(m_lock);
    bool waitLocked() WTF_REQUIRES_LOCK(m_lock);

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
    RefPtr<PlatformMediaResource> m_resource WTF_GUARDED_BY_LOCK(m_lock);
};

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
