// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "FFmpegResourceIO.h"

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "FFmpegLog.h"
#include "FFmpegPlaybackPolicy.h"
#include "HTTPHeaderNames.h"
#include "HTTPStatusCodes.h"
#include "PlatformMediaResourceLoader.h"
#include "ResourceError.h"
#include "ResourceRequest.h"
#include "ResourceResponse.h"
#include "SharedBuffer.h"
#include <wtf/MainThread.h>
#include <wtf/MonotonicTime.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace WebCore {

namespace {

// Network read-ahead per stream: see networkShouldSuspend/Resume in
// FFmpegPlaybackPolicy.h (16 MiB / 4 MiB, 64 MiB without Range support).
// Already-read data kept for the short backward seeks demuxers do.
constexpr uint64_t historyBytes = 512u << 10;
// A forward seek this close to the received data waits for it instead of
// issuing a new request.
constexpr uint64_t forwardWaitBytes = 512u << 10;
// Received data is coalesced into chunks of about this size.
constexpr size_t chunkCoalesceBytes = 256u << 10;
constexpr Seconds pollInterval = 50_ms;
constexpr int avioBufferSize = 64 * 1024;

class StreamClient final : public PlatformMediaResourceClient {
public:
    static Ref<StreamClient> create(Ref<FFmpegResourceStream>&& stream, unsigned requestId)
    {
        return adoptRef(*new StreamClient(WTF::move(stream), requestId));
    }

private:
    StreamClient(Ref<FFmpegResourceStream>&& stream, unsigned requestId)
        : m_stream(WTF::move(stream))
        , m_requestId(requestId)
    {
    }

    void responseReceived(PlatformMediaResource& resource, const ResourceResponse& response, CompletionHandler<void(ShouldContinuePolicyCheck)>&& completionHandler) final
    {
        bool shouldContinue = true;
        m_stream->didReceiveResponse(m_requestId, response, resource.didPassAccessControlCheck(), shouldContinue);
        completionHandler(shouldContinue ? ShouldContinuePolicyCheck::Yes : ShouldContinuePolicyCheck::No);
    }

    bool shouldCacheResponse(PlatformMediaResource&, const ResourceResponse&) final { return false; }
    void dataReceived(PlatformMediaResource&, const SharedBuffer& buffer) final { m_stream->didReceiveData(m_requestId, buffer.span()); }
    void accessControlCheckFailed(PlatformMediaResource&, const ResourceError&) final { m_stream->didFail(m_requestId, true); }
    void loadFailed(PlatformMediaResource&, const ResourceError&) final { m_stream->didFail(m_requestId, false); }
    void loadFinished(PlatformMediaResource&, const NetworkLoadMetrics&) final { m_stream->didFinish(m_requestId); }

    const Ref<FFmpegResourceStream> m_stream;
    const unsigned m_requestId;
};

// "bytes 0-99/1234" -> 1234; -1 when absent or "*".
int64_t totalFromContentRange(const String& contentRange)
{
    size_t slash = contentRange.find('/');
    if (slash == notFound)
        return -1;
    auto total = parseIntegerAllowingTrailingJunk<uint64_t>(StringView(contentRange).substring(slash + 1));
    return total ? static_cast<int64_t>(*total) : -1;
}

double monotonicSeconds()
{
    return MonotonicTime::now().secondsSinceEpoch().seconds();
}

int readPacketCallback(void* opaque, uint8_t* buffer, int size)
{
    return static_cast<FFmpegResourceStream*>(opaque)->read(buffer, size);
}

int64_t seekCallback(void* opaque, int64_t offset, int whence)
{
    return static_cast<FFmpegResourceStream*>(opaque)->seek(offset, whence);
}

} // namespace

Ref<FFmpegResourceStream> FFmpegResourceStream::create(Ref<PlatformMediaResourceLoader>&& loader, const URL& url, Ref<FFmpegIOShared>&& shared)
{
    return adoptRef(*new FFmpegResourceStream(WTF::move(loader), url, WTF::move(shared)));
}

FFmpegResourceStream::FFmpegResourceStream(Ref<PlatformMediaResourceLoader>&& loader, const URL& url, Ref<FFmpegIOShared>&& shared)
    : m_loader(WTF::move(loader))
    , m_url(url.isolatedCopy())
    , m_shared(WTF::move(shared))
{
}

FFmpegResourceStream::~FFmpegResourceStream()
{
    Locker locker { m_lock };
    stopResourceLocked();
}

AVIOContext* FFmpegResourceStream::createAVIOContext(Ref<FFmpegResourceStream>&& stream)
{
    auto* buffer = static_cast<unsigned char*>(av_malloc(avioBufferSize));
    if (!buffer)
        return nullptr;
    bool seekable;
    {
        Locker locker { stream->m_lock };
        seekable = stream->m_size > 0;
    }
    auto* rawStream = &stream.leakRef();
    AVIOContext* context = avio_alloc_context(buffer, avioBufferSize, 0, rawStream, readPacketCallback, nullptr, seekCallback);
    if (!context) {
        av_free(buffer);
        rawStream->close();
        rawStream->deref();
        return nullptr;
    }
    // Servers without Range support stay seekable: a seek restarts the
    // download and skips to the target, which is slow but correct (an MP4
    // with the moov atom at the end needs it).
    context->seekable = seekable ? AVIO_SEEKABLE_NORMAL : 0;
    return context;
}

bool FFmpegResourceStream::isOurAVIOContext(const AVIOContext* context)
{
    return context && context->read_packet == readPacketCallback;
}

void FFmpegResourceStream::destroyAVIOContext(AVIOContext* context)
{
    if (!context)
        return;
    if (auto* stream = static_cast<FFmpegResourceStream*>(context->opaque)) {
        stream->close();
        stream->deref();
    }
    av_freep(&context->buffer);
    avio_context_free(&context);
}

void FFmpegResourceStream::setByteRange(uint64_t offset, uint64_t endOffset)
{
    Locker locker { m_lock };
    m_readPosition = offset;
    m_rangeEnd = endOffset > offset ? endOffset : 0;
    m_byteRangeEnd = m_rangeEnd;
}

std::string FFmpegResourceStream::hostForLog() const
{
    return m_url.host().utf8().legacyCStringPointer();
}

int FFmpegResourceStream::open()
{
    Locker locker { m_lock };
    startRequestLocked(m_readPosition, false);
    while (m_state == RequestState::Pending || m_state == RequestState::RetryWait) {
        if (!waitLocked())
            return AVERROR_EXIT;
    }
    if (m_state == RequestState::Failed)
        return AVERROR(EIO);
    return 0;
}

uint64_t FFmpegResourceStream::bufferStartLocked() const
{
    return m_chunks.isEmpty() ? m_requestPosition : m_chunks.first().position;
}

uint64_t FFmpegResourceStream::bufferEndLocked() const
{
    return m_chunks.isEmpty() ? m_requestPosition : m_chunks.last().end();
}

size_t FFmpegResourceStream::copyOutLocked(uint8_t* out, size_t size)
{
    size_t copied = 0;
    for (auto& chunk : m_chunks) {
        if (copied == size)
            break;
        if (m_readPosition < chunk.position || m_readPosition >= chunk.end())
            continue;
        size_t offset = static_cast<size_t>(m_readPosition - chunk.position);
        size_t count = std::min(size - copied, chunk.data.size() - offset);
        memcpy(out + copied, chunk.data.span().subspan(offset, count).data(), count);
        copied += count;
        m_readPosition += count;
    }
    return copied;
}

void FFmpegResourceStream::trimHistoryLocked()
{
    while (m_chunks.size() > 1 && m_chunks.first().end() + historyBytes <= m_readPosition)
        m_chunks.removeFirst();
}

bool FFmpegResourceStream::waitLocked()
{
    if (m_shared->aborted || m_closed)
        return false;
    m_condition.waitFor(m_lock, pollInterval);
    if (m_shared->aborted || m_closed)
        return false;
    checkRequestLocked();
    return true;
}

void FFmpegResourceStream::checkRequestLocked()
{
    double now = monotonicSeconds();
    if (m_state == RequestState::RetryWait) {
        if (now >= m_retryAt)
            restartRequestLocked();
        return;
    }
    if ((m_state == RequestState::Pending || m_state == RequestState::Receiving) && FFmpegMedia::networkStalled(m_lastProgressAt, now))
        retryOrFailLocked("stall 8s");
}

void FFmpegResourceStream::retryOrFailLocked(const char* reason)
{
    unsigned long long at = m_requestPosition + m_receivedInRequest;
    // Whatever the current request still delivers is ignored from now on.
    ++m_requestId;
    stopResourceLocked();
    auto decision = m_retry.onFailure();
    if (!decision.retry) {
        ffmpegMediaLog("net: %s on %s at byte %llu, giving up after %u retries", reason, hostForLog().c_str(), at, FFmpegMedia::networkMaxRetries);
        m_state = RequestState::Failed;
        m_shared->networkError = true;
        m_condition.notifyAll();
        return;
    }
    ffmpegMediaLog("net: %s on %s at byte %llu, retry %u/%u in %.1f s", reason, hostForLog().c_str(), at, decision.attempt,
        FFmpegMedia::networkMaxRetries, decision.delaySeconds);
    m_state = RequestState::RetryWait;
    m_retryAt = monotonicSeconds() + decision.delaySeconds;
    m_condition.notifyAll();
}

void FFmpegResourceStream::restartRequestLocked()
{
    // From the end of the received data when the reader is inside it (the
    // buffer stays), else from the read position.
    uint64_t start = bufferStartLocked();
    uint64_t end = bufferEndLocked();
    bool continueBuffer = !m_chunks.isEmpty() && m_readPosition >= start && m_readPosition <= end;
    uint64_t position = continueBuffer ? end : m_readPosition;
    if (m_byteRangeEnd > position)
        m_rangeEnd = m_byteRangeEnd;
    ffmpegMediaLog("net: retry %u/%u: requesting %s from byte %llu", m_retry.attempts(), FFmpegMedia::networkMaxRetries, hostForLog().c_str(),
        static_cast<unsigned long long>(position));
    startRequestLocked(position, continueBuffer);
}

int FFmpegResourceStream::read(uint8_t* buffer, int size)
{
    if (size <= 0)
        return 0;
    Locker locker { m_lock };
    while (true) {
        if (m_shared->aborted || m_closed)
            return AVERROR_EXIT;

        uint64_t start = bufferStartLocked();
        uint64_t end = bufferEndLocked();
        if (m_readPosition >= start && m_readPosition < end) {
            size_t copied = copyOutLocked(buffer, static_cast<size_t>(size));
            trimHistoryLocked();
            if (m_state == RequestState::Suspended && FFmpegMedia::networkShouldResume(bufferEndLocked() - m_readPosition))
                startRequestLocked(bufferEndLocked(), true);
            return static_cast<int>(copied);
        }

        if (m_size >= 0 && m_readPosition >= static_cast<uint64_t>(m_size))
            return AVERROR_EOF;

        bool inReach = m_readPosition >= start && m_readPosition <= end + forwardWaitBytes;
        switch (m_state) {
        case RequestState::None:
            startRequestLocked(m_readPosition, false);
            break;
        case RequestState::Pending:
        case RequestState::Receiving:
            if (!inReach)
                startRequestLocked(m_readPosition, false);
            break;
        case RequestState::Finished:
            if (inReach)
                return AVERROR_EOF;
            startRequestLocked(m_readPosition, false);
            break;
        case RequestState::Failed:
            return AVERROR(EIO);
        case RequestState::Suspended:
            startRequestLocked(m_readPosition, m_readPosition == end);
            break;
        case RequestState::RetryWait:
            break;
        }
        if (m_interruptibleBySeek && m_shared->seekPending)
            return AVERROR(EINTR);
        if (!waitLocked())
            return AVERROR_EXIT;
    }
}

int64_t FFmpegResourceStream::seek(int64_t offset, int whence)
{
    Locker locker { m_lock };
    if (whence & AVSEEK_SIZE) {
        while (m_state == RequestState::Pending || m_state == RequestState::None || m_state == RequestState::RetryWait) {
            if (m_state == RequestState::None)
                startRequestLocked(m_readPosition, false);
            if (!waitLocked())
                return AVERROR_EXIT;
        }
        return m_size >= 0 ? m_size : AVERROR(ENOSYS);
    }

    int64_t target;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET:
        target = offset;
        break;
    case SEEK_CUR:
        target = static_cast<int64_t>(m_readPosition) + offset;
        break;
    case SEEK_END:
        if (m_size < 0)
            return AVERROR(ENOSYS);
        target = m_size + offset;
        break;
    default:
        return AVERROR(EINVAL);
    }
    if (target < 0)
        return AVERROR(EINVAL);
    // The request (if one is needed) is issued lazily by the next read().
    m_readPosition = static_cast<uint64_t>(target);
    return target;
}

void FFmpegResourceStream::close()
{
    Locker locker { m_lock };
    m_closed = true;
    ++m_requestId;
    stopResourceLocked();
    m_chunks.clear();
    m_condition.notifyAll();
}

void FFmpegResourceStream::startRequestLocked(uint64_t position, bool keepBuffer)
{
    stopResourceLocked();
    if (!keepBuffer)
        m_chunks.clear();
    m_requestPosition = position;
    m_receivedInRequest = 0;
    m_skipBytes = 0;
    m_state = RequestState::Pending;
    m_lastProgressAt = monotonicSeconds();
    unsigned requestId = ++m_requestId;
    // The byte range end only bounds the first request (a segment); later
    // requests (resumes, seeks) are open-ended.
    uint64_t rangeEnd = std::exchange(m_rangeEnd, 0);
    if (rangeEnd <= position)
        rangeEnd = 0;
    unsigned count = ++m_shared->requestCount;
    if (position)
        ffmpegMediaLog("net: request #%u bytes=%llu-%s", count, static_cast<unsigned long long>(position),
            rangeEnd ? makeString(rangeEnd - 1).utf8().legacyCStringPointer() : "");
    callOnMainThread([protectedThis = Ref { *this }, requestId, position, rangeEnd] {
        protectedThis->dispatchRequest(requestId, position, rangeEnd);
    });
}

void FFmpegResourceStream::dispatchRequest(unsigned requestId, uint64_t position, uint64_t rangeEnd)
{
    ASSERT(isMainThread());
    {
        Locker locker { m_lock };
        if (m_closed || requestId != m_requestId)
            return;
    }

    ResourceRequest request { URL { m_url } };
    request.setAllowCookies(true);
    if (rangeEnd)
        request.setHTTPHeaderField(HTTPHeaderName::Range, makeString("bytes="_s, position, '-', rangeEnd - 1));
    else if (position)
        request.setHTTPHeaderField(HTTPHeaderName::Range, makeString("bytes="_s, position, '-'));

    // Never call into the resource while holding m_lock: shutdown() and the
    // loader may call the client synchronously, which takes m_lock.
    RefPtr resource = m_loader->requestResource(WTF::move(request), PlatformMediaResourceLoader::LoadOption::DisallowCaching);
    if (!resource) {
        ffmpegMediaLog("net: the media resource loader refused the request (%s)", m_url.protocol().utf8().legacyCStringPointer());
        Locker locker { m_lock };
        if (requestId == m_requestId) {
            m_state = RequestState::Failed;
            m_shared->networkError = true;
            m_condition.notifyAll();
        }
        return;
    }
    resource->setClient(StreamClient::create(Ref { *this }, requestId));

    bool stale;
    {
        Locker locker { m_lock };
        stale = m_closed || requestId != m_requestId;
        if (!stale)
            m_resource = resource;
    }
    if (stale) {
        resource->shutdown();
        resource->setClient(nullptr);
    }
}

void FFmpegResourceStream::stopResourceLocked()
{
    RefPtr resource = WTF::move(m_resource);
    if (!resource)
        return;
    // Always asynchronous: this runs on the demuxer thread or inside a
    // resource client callback.
    callOnMainThread([resource = WTF::move(resource)] {
        resource->shutdown();
        resource->setClient(nullptr);
    });
}

void FFmpegResourceStream::suspendLocked()
{
    ++m_requestId;
    m_state = RequestState::Suspended;
    stopResourceLocked();
}

void FFmpegResourceStream::didReceiveResponse(unsigned requestId, const ResourceResponse& response, bool passedAccessControlCheck, bool& shouldContinue)
{
    Locker locker { m_lock };
    if (m_closed || requestId != m_requestId) {
        shouldContinue = false;
        return;
    }

    bool isHTTP = response.url().protocolIsInHTTPFamily() || m_url.protocolIsInHTTPFamily();
    int status = response.httpStatusCode();
    m_lastProgressAt = monotonicSeconds();
    if (isHTTP && status >= httpStatus400BadRequest) {
        ffmpegMediaLog("net: HTTP %d for %s://%s at byte %llu", status, m_url.protocol().utf8().legacyCStringPointer(), hostForLog().c_str(),
            static_cast<unsigned long long>(m_requestPosition));
        shouldContinue = false;
        if (FFmpegMedia::networkStatusIsRetryable(status)) {
            char reason[16];
            snprintf(reason, sizeof(reason), "HTTP %d", status);
            retryOrFailLocked(reason);
            return;
        }
        m_state = RequestState::Failed;
        m_shared->networkError = true;
        m_condition.notifyAll();
        return;
    }
    if (m_retry.attempts())
        ffmpegMediaLog("net: retry %u/%u: HTTP %d from %s", m_retry.attempts(), FFmpegMedia::networkMaxRetries, status, hostForLog().c_str());

    m_shared->sawResponse = true;
    if (isHTTP && !passedAccessControlCheck)
        m_shared->didPassAccessControlCheck = false;

    bool isPartial = isHTTP && status == httpStatus206PartialContent;
    if (m_requestPosition && !isPartial) {
        // The server ignored Range and sends the whole body: skip to the
        // requested position and stop issuing range requests.
        m_skipBytes = m_requestPosition;
        if (m_rangesSupported)
            ffmpegMediaLog("net: %s ignored Range (HTTP %d), skipping %llu bytes", m_url.host().utf8().legacyCStringPointer(), status,
                static_cast<unsigned long long>(m_requestPosition));
        m_rangesSupported = false;
    }

    int64_t length = response.httpHeaderFields().contains(HTTPHeaderName::ContentEncoding) ? -1 : response.expectedContentLength();
    if (isPartial) {
        int64_t total = totalFromContentRange(response.httpHeaderField(HTTPHeaderName::ContentRange));
        if (total > 0)
            m_size = total;
        else if (length > 0)
            m_size = static_cast<int64_t>(m_requestPosition) + length;
    } else if (length > 0)
        m_size = length;

    if (isHTTP && equalLettersIgnoringASCIICase(response.httpHeaderField(HTTPHeaderName::AcceptRanges), "none"_s))
        m_rangesSupported = false;

    m_state = RequestState::Receiving;
    m_condition.notifyAll();
}

void FFmpegResourceStream::didReceiveData(unsigned requestId, std::span<const uint8_t> data)
{
    Locker locker { m_lock };
    if (m_closed || requestId != m_requestId)
        return;

    m_shared->bytesReceived += data.size();
    m_lastProgressAt = monotonicSeconds();
    if (m_retry.attempts()) {
        ffmpegMediaLog("net: %s recovered at byte %llu after %u retries", hostForLog().c_str(),
            static_cast<unsigned long long>(m_requestPosition + m_receivedInRequest), m_retry.attempts());
        m_retry.onProgress();
    }
    if (m_skipBytes) {
        size_t skipped = static_cast<size_t>(std::min<uint64_t>(m_skipBytes, data.size()));
        m_skipBytes -= skipped;
        data = data.subspan(skipped);
    }
    if (data.empty())
        return;

    uint64_t position = m_requestPosition + m_receivedInRequest;
    m_receivedInRequest += data.size();
    if (!m_chunks.isEmpty() && m_chunks.last().end() == position && m_chunks.last().data.size() < chunkCoalesceBytes)
        m_chunks.last().data.append(data);
    else {
        Chunk chunk;
        chunk.position = position;
        chunk.data.append(data);
        m_chunks.append(WTF::move(chunk));
    }

    // Also with an unknown size or without Range support: an unbounded
    // buffer would hold a whole long progressive download in memory. A
    // resume without Range support restarts the request and skips ahead.
    uint64_t end = bufferEndLocked();
    if (end > m_readPosition && FFmpegMedia::networkShouldSuspend(end - m_readPosition, end, m_size, m_rangesSupported))
        suspendLocked();
    m_condition.notifyAll();
}

void FFmpegResourceStream::didFinish(unsigned requestId)
{
    Locker locker { m_lock };
    if (m_closed || requestId != m_requestId)
        return;
    m_state = RequestState::Finished;
    // Without Content-Length the size is known once the body is complete.
    if (m_size < 0 && !m_skipBytes)
        m_size = static_cast<int64_t>(m_requestPosition + m_receivedInRequest);
    m_condition.notifyAll();
}

void FFmpegResourceStream::didFail(unsigned requestId, bool isAccessControlFailure)
{
    Locker locker { m_lock };
    if (m_closed || requestId != m_requestId)
        return;
    ffmpegMediaLog("net: load failed%s for %s://%s at byte %llu", isAccessControlFailure ? " (CORS)" : "",
        m_url.protocol().utf8().legacyCStringPointer(), hostForLog().c_str(),
        static_cast<unsigned long long>(m_requestPosition + m_receivedInRequest));
    if (!isAccessControlFailure) {
        retryOrFailLocked("load failed");
        return;
    }
    m_state = RequestState::Failed;
    m_shared->networkError = true;
    if (isAccessControlFailure)
        m_shared->didPassAccessControlCheck = false;
    m_condition.notifyAll();
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
