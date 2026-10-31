// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "FFmpegDemuxer.h"

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "FFmpegLog.h"
#include "Logging.h"
#include "PlatformMediaResourceLoader.h"
#include <wtf/MainThread.h>
#include <wtf/MonotonicTime.h>

namespace WebCore {

namespace {

constexpr Seconds idlePollInterval = 20_ms;
// Consecutive non-fatal read errors tolerated before giving up.
constexpr unsigned maxConsecutiveReadErrors = 64;
// HLS variant cap: software decoding above 1080p is not realistic.
constexpr int maxVideoWidth = 1920;
constexpr int maxVideoHeight = 1088;

int64_t streamBitRate(const AVStream* stream)
{
    if (stream->codecpar->bit_rate > 0)
        return stream->codecpar->bit_rate;
    if (auto* entry = av_dict_get(stream->metadata, "variant_bitrate", nullptr, 0))
        return strtoll(entry->value, nullptr, 10);
    return 0;
}

} // namespace

// MARK: - FFmpegPacketQueue

void FFmpegPacketQueue::push(AVPacketPtr&& packet, unsigned serial)
{
    Locker locker { m_lock };
    if (m_aborted || serial != m_serial)
        return;
    m_bytes += packet->size;
    int64_t timestamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
    if (timestamp != AV_NOPTS_VALUE) {
        int64_t end = timestamp + std::max<int64_t>(packet->duration, 0);
        if (m_lastTimestamp == AV_NOPTS_VALUE || end > m_lastTimestamp)
            m_lastTimestamp = end;
    }
    m_entries.append({ WTF::move(packet), serial });
    m_condition.notifyAll();
}

void FFmpegPacketQueue::pushEndOfStream(unsigned serial)
{
    Locker locker { m_lock };
    if (m_aborted || serial != m_serial)
        return;
    m_entries.append({ nullptr, serial });
    m_condition.notifyAll();
}

std::optional<FFmpegPacketQueue::Entry> FFmpegPacketQueue::pop(Seconds timeout)
{
    Locker locker { m_lock };
    if (m_entries.isEmpty() && !m_aborted)
        m_condition.waitFor(m_lock, timeout);
    if (m_aborted || m_entries.isEmpty())
        return std::nullopt;
    auto entry = m_entries.takeFirst();
    if (entry.packet)
        m_bytes -= std::min<size_t>(m_bytes, entry.packet->size);
    if (m_entries.isEmpty())
        m_lastTimestamp = AV_NOPTS_VALUE;
    return entry;
}

void FFmpegPacketQueue::flush(unsigned newSerial)
{
    Locker locker { m_lock };
    m_entries.clear();
    m_bytes = 0;
    m_lastTimestamp = AV_NOPTS_VALUE;
    m_serial = newSerial;
    m_condition.notifyAll();
}

void FFmpegPacketQueue::abort()
{
    Locker locker { m_lock };
    m_aborted = true;
    m_entries.clear();
    m_bytes = 0;
    m_condition.notifyAll();
}

size_t FFmpegPacketQueue::bytes() const
{
    Locker locker { m_lock };
    return m_bytes;
}

FFmpegMedia::QueueLevel FFmpegPacketQueue::level() const
{
    Locker locker { m_lock };
    FFmpegMedia::QueueLevel level;
    level.enabled = m_enabled;
    level.packets = m_entries.size();
    if (m_entries.isEmpty() || m_lastTimestamp == AV_NOPTS_VALUE)
        return level;
    for (auto& entry : m_entries) {
        if (!entry.packet)
            continue;
        int64_t first = entry.packet->pts != AV_NOPTS_VALUE ? entry.packet->pts : entry.packet->dts;
        if (first == AV_NOPTS_VALUE)
            continue;
        level.seconds = std::max(0.0, (m_lastTimestamp - first) * av_q2d(m_timeBase));
        break;
    }
    return level;
}

// MARK: - FFmpegDemuxer

Ref<FFmpegDemuxer> FFmpegDemuxer::create(const URL& url, Ref<PlatformMediaResourceLoader>&& loader, const String& userAgent)
{
    return adoptRef(*new FFmpegDemuxer(url, WTF::move(loader), userAgent));
}

FFmpegDemuxer::FFmpegDemuxer(const URL& url, Ref<PlatformMediaResourceLoader>&& loader, const String& userAgent)
    : m_url(url.isolatedCopy())
    , m_loader(WTF::move(loader))
    , m_userAgent(userAgent.isolatedCopy())
    , m_shared(FFmpegIOShared::create())
{
}

FFmpegDemuxer::~FFmpegDemuxer()
{
    // The thread holds a reference, so it has finished by now.
    ASSERT(!m_context);
}

void FFmpegDemuxer::start()
{
    ASSERT(isMainThread());
    if (m_thread)
        return;
    m_thread = Thread::create("FFmpegDemuxer"_s, [protectedThis = Ref { *this }] {
        protectedThis->run();
    });
}

void FFmpegDemuxer::stop()
{
    ASSERT(isMainThread());
    m_shared->aborted = true;
    m_videoQueue.abort();
    m_audioQueue.abort();
    {
        Locker locker { m_lock };
        m_condition.notifyAll();
    }
    if (RefPtr thread = std::exchange(m_thread, nullptr))
        thread->waitForCompletion();
}

FFmpegDemuxer::Status FFmpegDemuxer::status() const
{
    Status status;
    Locker locker { m_lock };
    status.state = m_state;
    status.failure = m_failure;
    status.endOfStream = m_endOfStream;
    status.readerWaiting = m_readerWaiting;
    status.bytesReceived = m_shared->bytesReceived;
    status.buffered = m_buffered;
    status.seekFailedSerial = m_seekFailedSerial;
    status.seekDoneSerial = m_seekDoneSerial;
    return status;
}

void FFmpegDemuxer::seek(double time, unsigned serial)
{
    Locker locker { m_lock };
    m_pendingSeek = std::make_pair(time, serial);
    m_endOfStream = false;
    // Wakes a top-level read that waits for data at the old position.
    m_shared->seekPending = true;
    m_condition.notifyAll();
}

int FFmpegDemuxer::interruptCallback(void* opaque)
{
    return static_cast<FFmpegDemuxer*>(opaque)->m_shared->aborted ? 1 : 0;
}

bool FFmpegDemuxer::usesResourceLoader(const URL& url) const
{
    // Everything WebCore can load goes through its loader (cookies, CORS,
    // CSP, blob: URLs). Compound URLs such as HLS "crypto+https://" (AES-128
    // segments) use ffmpeg's own protocols: no cookies there.
    return url.isValid() && (url.protocolIsInHTTPFamily() || url.protocolIsBlob() || url.protocolIsFile() || url.protocolIsData());
}

int FFmpegDemuxer::ioOpen(AVFormatContext* context, AVIOContext** pb, const char* url, int flags, AVDictionary** options)
{
    auto* demuxer = static_cast<FFmpegDemuxer*>(context->opaque);
    URL parsed { String::fromUTF8(url) };
    if (parsed.protocolIs("skd"_s)) {
        // FairPlay (HLS SAMPLE-AES with an Apple key server): no key can
        // ever be fetched, and libavformat would go on "decrypting" the
        // segments with a zero key and feed garbage to the decoders.
        if (!demuxer->m_shared->fairPlayKeyRequested.exchange(true))
            ffmpegMediaLog("demuxer: HLS key %s:// is FairPlay DRM, which this engine cannot play", "skd");
        return AVERROR(EPERM);
    }
    if (!(flags & AVIO_FLAG_WRITE) && demuxer->usesResourceLoader(parsed)) {
        auto stream = FFmpegResourceStream::create(demuxer->m_loader.copyRef(), parsed, demuxer->m_shared.copyRef());
        // HLS EXT-X-BYTERANGE: libavformat passes the segment's byte range as
        // options for its http protocol and, believing the URL is http, does
        // not seek to it itself.
        if (options && *options) {
            auto* offset = av_dict_get(*options, "offset", nullptr, 0);
            auto* endOffset = av_dict_get(*options, "end_offset", nullptr, 0);
            uint64_t start = offset ? strtoull(offset->value, nullptr, 10) : 0;
            uint64_t end = endOffset ? strtoull(endOffset->value, nullptr, 10) : 0;
            if (start || end)
                stream->setByteRange(start, end);
        }
        int result = stream->open();
        if (result < 0) {
            if (result != AVERROR_EXIT)
                ffmpegMediaLog("demuxer: open %s://%s failed: %s", parsed.protocol().utf8().legacyCStringPointer(),
                    parsed.host().utf8().legacyCStringPointer(), ffmpegErrorString(result).utf8().legacyCStringPointer());
            stream->close();
            return result;
        }
        *pb = FFmpegResourceStream::createAVIOContext(WTF::move(stream));
        return *pb ? 0 : AVERROR(ENOMEM);
    }
    if (!demuxer->m_defaultIoOpen)
        return AVERROR(ENOSYS);
    return demuxer->m_defaultIoOpen(context, pb, url, flags, options);
}

int FFmpegDemuxer::ioClose(AVFormatContext* context, AVIOContext* pb)
{
    if (FFmpegResourceStream::isOurAVIOContext(pb)) {
        FFmpegResourceStream::destroyAVIOContext(pb);
        return 0;
    }
    auto* demuxer = static_cast<FFmpegDemuxer*>(context->opaque);
    if (demuxer && demuxer->m_defaultIoClose)
        return demuxer->m_defaultIoClose(context, pb);
    return 0;
}

void FFmpegDemuxer::fail(FFmpegMedia::LoadFailure failure)
{
    m_videoQueue.abort();
    m_audioQueue.abort();
    Locker locker { m_lock };
    m_state = State::Failed;
    if (m_failure == FFmpegMedia::LoadFailure::None)
        m_failure = failure;
}

void FFmpegDemuxer::run()
{
    if (openInput()) {
        selectStreams();
        logStreams();
        if (m_shared->fairPlayKeyRequested) {
            ffmpegMediaLog("demuxer: refusing the stream: it is FairPlay-encrypted");
            fail(FFmpegMedia::LoadFailure::Open);
        } else if (!m_video.parameters && !m_audio.parameters) {
            ffmpegMediaLog("demuxer: no decodable stream");
            fail(FFmpegMedia::LoadFailure::Open);
        } else {
            {
                Locker locker { m_lock };
                m_state = State::Ready;
            }
            readLoop();
        }
    }
    closeInput();
}

bool FFmpegDemuxer::openInput()
{
    m_context = avformat_alloc_context();
    if (!m_context) {
        fail(FFmpegMedia::LoadFailure::Open);
        return false;
    }
    m_context->interrupt_callback.callback = interruptCallback;
    m_context->interrupt_callback.opaque = this;
    m_context->opaque = this;
    m_defaultIoOpen = m_context->io_open;
    m_defaultIoClose = m_context->io_close2;
    m_context->io_open = ioOpen;
    m_context->io_close2 = ioClose;

    if (usesResourceLoader(m_url)) {
        // The top-level context is custom I/O: avformat_close_input() would
        // free a non-custom pb with avio_close(), which assumes a URLContext.
        auto stream = FFmpegResourceStream::create(m_loader.copyRef(), m_url, m_shared.copyRef());
        stream->setInterruptibleBySeek(true);
        int result = stream->open();
        if (result < 0) {
            stream->close();
            if (!m_shared->aborted) {
                ffmpegMediaLog("demuxer: first request failed: %s", ffmpegErrorString(result).utf8().legacyCStringPointer());
                fail(FFmpegMedia::LoadFailure::Network);
            }
            return false;
        }
        m_topLevelIO = FFmpegResourceStream::createAVIOContext(WTF::move(stream));
        if (!m_topLevelIO) {
            fail(FFmpegMedia::LoadFailure::Open);
            return false;
        }
        m_context->pb = m_topLevelIO;
        m_context->flags |= AVFMT_FLAG_CUSTOM_IO;
    }

    AVDictionary* options = nullptr;
    // hls: our AVIOContexts are not ffmpeg http contexts, so connection reuse
    // (which pokes at URLContext internals) must stay off.
    av_dict_set(&options, "http_persistent", "0", 0);
    av_dict_set(&options, "http_multiple", "0", 0);
    // Only used by ffmpeg's own protocols (see usesResourceLoader()).
    if (!m_userAgent.isEmpty())
        av_dict_set(&options, "user_agent", m_userAgent.utf8().legacyCStringPointer(), 0);

    auto urlString = m_url.string().utf8();
    int result = avformat_open_input(&m_context, urlString.legacyCStringPointer(), nullptr, &options);
    av_dict_free(&options);
    if (result < 0) {
        // avformat_open_input() freed the context on failure.
        m_context = nullptr;
        if (!m_shared->aborted) {
            ffmpegMediaLog("demuxer: avformat_open_input failed: %s", ffmpegErrorString(result).utf8().legacyCStringPointer());
            fail(m_shared->networkError ? FFmpegMedia::LoadFailure::Network : FFmpegMedia::LoadFailure::Open);
        }
        return false;
    }

    m_isHLS = m_context->iformat && m_context->iformat->name && strstr(m_context->iformat->name, "hls");
    result = avformat_find_stream_info(m_context, nullptr);
    if (result < 0 && !m_shared->aborted)
        ffmpegMediaLog("demuxer: avformat_find_stream_info: %s", ffmpegErrorString(result).utf8().legacyCStringPointer());
    if (m_shared->aborted)
        return false;

    if (m_context->duration != AV_NOPTS_VALUE && m_context->duration > 0)
        m_duration = m_context->duration / static_cast<double>(AV_TIME_BASE);
    else {
        // HLS without #EXT-X-ENDLIST, Icecast, live WebM: unbounded.
        m_duration = std::numeric_limits<double>::infinity();
        m_isLive = true;
    }
    bool ioSeekable = !m_context->pb || (m_context->pb->seekable & AVIO_SEEKABLE_NORMAL) || m_isHLS;
    m_isSeekable = !m_isLive && ioSeekable && !(m_context->ctx_flags & AVFMTCTX_UNSEEKABLE);
    return true;
}

void FFmpegDemuxer::selectStreams()
{
    double startTime = m_context->start_time != AV_NOPTS_VALUE ? m_context->start_time / static_cast<double>(AV_TIME_BASE) : 0;

    // Video: prefer the highest bit rate at or below 1080p (HLS exposes every
    // variant as a stream; there is no adaptive switching).
    int videoIndex = -1;
    bool bestFits = false;
    int64_t bestRate = -1;
    int bestHeight = 0;
    for (unsigned i = 0; i < m_context->nb_streams; ++i) {
        AVStream* stream = m_context->streams[i];
        auto* parameters = stream->codecpar;
        if (parameters->codec_type != AVMEDIA_TYPE_VIDEO || (stream->disposition & AV_DISPOSITION_ATTACHED_PIC))
            continue;
        if (!findSoftwareDecoder(parameters->codec_id))
            continue;
        bool fits = parameters->width <= maxVideoWidth && parameters->height <= maxVideoHeight;
        int64_t rate = streamBitRate(stream);
        bool better;
        if (videoIndex < 0)
            better = true;
        else if (fits != bestFits)
            better = fits;
        else if (fits)
            better = rate > bestRate || (rate == bestRate && parameters->height > bestHeight);
        else
            better = parameters->height < bestHeight;
        if (better) {
            videoIndex = static_cast<int>(i);
            bestFits = fits;
            bestRate = rate;
            bestHeight = parameters->height;
        }
    }

    int audioIndex = av_find_best_stream(m_context, AVMEDIA_TYPE_AUDIO, -1, videoIndex, nullptr, 0);
    if (audioIndex >= 0 && !findSoftwareDecoder(m_context->streams[audioIndex]->codecpar->codec_id))
        audioIndex = -1;

    auto fill = [&](FFmpegStreamInfo& info, FFmpegPacketQueue& queue, int index) {
        if (index < 0)
            return;
        AVStream* stream = m_context->streams[index];
        AVCodecParametersPtr parameters(avcodec_parameters_alloc());
        if (!parameters || avcodec_parameters_copy(parameters.get(), stream->codecpar) < 0)
            return;
        info.index = index;
        info.timeBase = stream->time_base;
        info.startTime = startTime;
        info.parameters = WTF::move(parameters);
        queue.setTimeBase(stream->time_base);
        queue.setEnabled(true);
    };
    fill(m_video, m_videoQueue, videoIndex);
    fill(m_audio, m_audioQueue, audioIndex);

    for (unsigned i = 0; i < m_context->nb_streams; ++i) {
        if (static_cast<int>(i) != m_video.index && static_cast<int>(i) != m_audio.index)
            m_context->streams[i]->discard = AVDISCARD_ALL;
    }
}

void FFmpegDemuxer::logStreams() const
{
    if (!ffmpegMediaLogEnabled() || !m_context)
        return;
    ffmpegMediaLog("demuxer: opened %s%s, duration %.3f s%s, start %.3f s, %u streams, %s", m_context->iformat && m_context->iformat->name ? m_context->iformat->name : "?",
        m_isHLS ? " (HLS)" : "", m_duration, m_isLive ? " (live)" : "", m_context->start_time != AV_NOPTS_VALUE ? m_context->start_time / static_cast<double>(AV_TIME_BASE) : 0.0,
        m_context->nb_streams, m_isSeekable ? "seekable" : "not seekable");
    for (unsigned i = 0; i < m_context->nb_streams && i < 32; ++i) {
        const AVStream* stream = m_context->streams[i];
        const AVCodecParameters* parameters = stream->codecpar;
        const char* type = av_get_media_type_string(parameters->codec_type);
        const char* use = static_cast<int>(i) == m_video.index ? "VIDEO" : static_cast<int>(i) == m_audio.index ? "AUDIO" : "-";
        if (parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
            ffmpegMediaLog("demuxer:  #%u %s %s %dx%d %lld b/s%s [%s]", i, type ? type : "?", avcodec_get_name(parameters->codec_id), parameters->width,
                parameters->height, static_cast<long long>(streamBitRate(stream)), findSoftwareDecoder(parameters->codec_id) ? "" : " (no decoder)", use);
        } else {
            ffmpegMediaLog("demuxer:  #%u %s %s %d Hz %d ch %lld b/s%s [%s]", i, type ? type : "?", avcodec_get_name(parameters->codec_id), parameters->sample_rate,
                parameters->ch_layout.nb_channels, static_cast<long long>(streamBitRate(stream)), findSoftwareDecoder(parameters->codec_id) ? "" : " (no decoder)", use);
        }
    }
}

int FFmpegDemuxer::seekTo(double time)
{
    const FFmpegStreamInfo& primary = m_video.index >= 0 ? m_video : m_audio;
    int64_t target = static_cast<int64_t>((time + primary.startTime) * AV_TIME_BASE);
    // Land on the keyframe at or before the target; the decoders discard
    // what precedes the target (accurate seek).
    int result = avformat_seek_file(m_context, -1, std::numeric_limits<int64_t>::min(), target, target, 0);
    if (result >= 0 || primary.index < 0)
        return result;
    ffmpegMediaLog("demuxer: avformat_seek_file(%.3f) failed: %s, retrying on stream %d", time,
        ffmpegErrorString(result).utf8().legacyCStringPointer(), primary.index);
    int64_t streamTarget = av_rescale_q(target, AV_TIME_BASE_Q, primary.timeBase);
    result = av_seek_frame(m_context, primary.index, streamTarget, AVSEEK_FLAG_BACKWARD);
    if (result >= 0)
        return result;
    // Any frame, not only a keyframe: the video decoder then waits for the
    // next keyframe, which beats not seeking at all.
    return av_seek_frame(m_context, primary.index, streamTarget, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
}

void FFmpegDemuxer::performSeek(double time, unsigned serial)
{
    auto start = MonotonicTime::now();
    // An interrupted read (seekPending) left its error in the top-level
    // AVIOContext; libavio reports a stored error instead of EOF later.
    if (m_topLevelIO)
        m_topLevelIO->error = 0;
    int result = seekTo(time);
    m_serial = serial;
    m_videoQueue.flush(serial);
    m_audioQueue.flush(serial);
    m_segmentStart = time;
    {
        Locker locker { m_lock };
        // The read in flight when seek() was called may have hit the end of
        // the stream after seek() cleared the flag: without this, the loop
        // would idle on m_endOfStream and the new serial would never get a
        // packet (playback frozen after the seek).
        m_endOfStream = false;
        if (!m_pendingSeek)
            m_shared->seekPending = false;
        m_seekDoneSerial = serial;
        if (result < 0)
            m_seekFailedSerial = serial;
    }
    if (result < 0)
        ffmpegMediaLog("demuxer: seek #%u to %.3f FAILED: %s; reading on from the old position", serial, time,
            ffmpegErrorString(result).utf8().legacyCStringPointer());
    else
        ffmpegMediaLog("demuxer: seek #%u to %.3f done in %.0f ms, queues flushed", serial, time, (MonotonicTime::now() - start).milliseconds());
}

void FFmpegDemuxer::readLoop()
{
    AVPacketPtr packet(av_packet_alloc());
    if (!packet) {
        fail(FFmpegMedia::LoadFailure::Open);
        return;
    }
    unsigned consecutiveErrors = 0;
    const FFmpegStreamInfo& primary = m_video.index >= 0 ? m_video : m_audio;

    while (!m_shared->aborted) {
        std::optional<std::pair<double, unsigned>> seekRequest;
        bool shouldWait;
        {
            Locker locker { m_lock };
            seekRequest = std::exchange(m_pendingSeek, std::nullopt);
            shouldWait = !seekRequest && (m_endOfStream
                || FFmpegMedia::demuxerShouldWait(m_videoQueue.bytes() + m_audioQueue.bytes(), m_videoQueue.level(), m_audioQueue.level(),
                    m_isHLS ? FFmpegMedia::hlsQueuedSeconds : FFmpegMedia::maxQueuedSeconds));
            m_readerWaiting = shouldWait;
            if (shouldWait) {
                m_condition.waitFor(m_lock, idlePollInterval);
                continue;
            }
        }
        if (seekRequest) {
            performSeek(seekRequest->first, seekRequest->second);
            consecutiveErrors = 0;
            continue;
        }

        int result = av_read_frame(m_context, packet.get());
        if (result < 0) {
            if (m_shared->aborted)
                break;
            if (result == AVERROR(EINTR) && m_shared->seekPending) {
                // A read gave way to the pending seek (FFmpegResourceStream).
                av_packet_unref(packet.get());
                continue;
            }
            if (result == AVERROR_EXIT) {
                ffmpegMediaLog("demuxer: av_read_frame: %s, stopping", ffmpegErrorString(result).utf8().legacyCStringPointer());
                break;
            }
            bool endOfFile = result == AVERROR_EOF || (m_context->pb && avio_feof(m_context->pb));
            if (!endOfFile && ++consecutiveErrors < maxConsecutiveReadErrors) {
                if (consecutiveErrors == 1 || !(consecutiveErrors % 16))
                    ffmpegMediaLog("demuxer: av_read_frame: %s (%u in a row)", ffmpegErrorString(result).utf8().legacyCStringPointer(), consecutiveErrors);
                Locker locker { m_lock };
                m_condition.waitFor(m_lock, 10_ms);
                continue;
            }
            ffmpegMediaLog("demuxer: %s (serial %u): %s", endOfFile ? "end of stream" : "giving up after read errors", m_serial,
                ffmpegErrorString(result).utf8().legacyCStringPointer());
            m_videoQueue.pushEndOfStream(m_serial);
            m_audioQueue.pushEndOfStream(m_serial);
            Locker locker { m_lock };
            m_endOfStream = true;
            if (m_shared->networkError && !endOfFile && m_failure == FFmpegMedia::LoadFailure::None)
                m_failure = FFmpegMedia::LoadFailure::Network;
            continue;
        }
        consecutiveErrors = 0;

        FFmpegPacketQueue* queue = nullptr;
        if (packet->stream_index == m_video.index)
            queue = &m_videoQueue;
        else if (packet->stream_index == m_audio.index)
            queue = &m_audioQueue;
        if (!queue) {
            av_packet_unref(packet.get());
            continue;
        }

        if (packet->stream_index == primary.index) {
            int64_t timestamp = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            double start = primary.toSeconds(timestamp);
            if (std::isfinite(start)) {
                double end = start + std::max<int64_t>(packet->duration, 0) * av_q2d(primary.timeBase);
                Locker locker { m_lock };
                m_buffered.add(std::min(m_segmentStart, start), std::max(end, start + 0.001));
            }
        }

        AVPacketPtr queued(av_packet_alloc());
        if (!queued)
            break;
        av_packet_move_ref(queued.get(), packet.get());
        queue->push(WTF::move(queued), m_serial);
    }
}

void FFmpegDemuxer::closeInput()
{
    if (m_context)
        avformat_close_input(&m_context);
    m_context = nullptr;
    // Custom top-level I/O is not closed by avformat_close_input().
    if (m_topLevelIO) {
        FFmpegResourceStream::destroyAVIOContext(m_topLevelIO);
        m_topLevelIO = nullptr;
    }
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
