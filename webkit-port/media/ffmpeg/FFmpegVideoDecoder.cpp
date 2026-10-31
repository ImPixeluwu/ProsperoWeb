// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "FFmpegVideoDecoder.h"

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "FFmpegDemuxer.h"
#include "FFmpegLog.h"
#include "Logging.h"
#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <wtf/MonotonicTime.h>
}

namespace WebCore {

namespace {

constexpr Seconds packetWaitTimeout = 20_ms;
constexpr Seconds outputWaitTimeout = 20_ms;
// Decoded frames waiting for presentation. Small: each 1080p BGRA frame is
// 8 MiB, and the presentation tick only ever needs the next one or two.
constexpr size_t maxOutputFrames = 3;
constexpr size_t maxPooledFrames = 5;
// Output is capped at 1080p; anything bigger is downscaled by swscale.
constexpr int maxOutputWidth = 1920;
constexpr int maxOutputHeight = 1080;
// Decoder threads: 6 of the Zen 2's 8 cores, leaving 2 for the UI process,
// networking and the audio decoder (PROSPEROBROWSER_MEDIA_THREADS=1..16).
// Frame and slice threading: frame threading is what scales H.264 and VP9
// across cores. The console crashes of run 4/5 that first pointed at the
// frame threads ("av:h264:df*") were the swscale overrun fixed in
// FFmpegPlaybackPolicy.h, not frame threading;
// PROSPEROBROWSER_MEDIA_FRAME_THREADS=0 goes back to slice threads only.
constexpr int defaultDecoderThreads = 6;

int decoderThreads()
{
    if (const char* value = getenv("PROSPEROBROWSER_MEDIA_THREADS")) {
        int count = atoi(value);
        if (count >= 1 && count <= 16)
            return count;
    }
    return defaultDecoderThreads;
}

bool frameThreadsEnabled()
{
    const char* value = getenv("PROSPEROBROWSER_MEDIA_FRAME_THREADS");
    return !value || strcmp(value, "0");
}

int64_t nowMicroseconds()
{
    return static_cast<int64_t>(MonotonicTime::now().secondsSinceEpoch().microseconds());
}
// A frame this far behind the clock is dropped before conversion; one in
// lateFrameKeepInterval late frames is still shown so the picture moves.
constexpr double lateThreshold = 0.08;
constexpr unsigned lateFrameKeepInterval = 8;
// Falling behind: first skip deblocking of non-reference frames, then of
// all frames.
constexpr unsigned lateFramesBeforeSkippingLoopFilter = 4;
constexpr unsigned lateFramesBeforeSkippingAllLoopFilter = 12;

int swsColorspace(const AVFrame* frame)
{
    switch (frame->colorspace) {
    case AVCOL_SPC_BT709:
        return SWS_CS_ITU709;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return SWS_CS_BT2020;
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_BT470BG:
        return SWS_CS_ITU601;
    default:
        // Unspecified: HD content is BT.709 in practice.
        return frame->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }
}

// Process-wide totals for ps5_media_video_decoder_stats().
std::atomic<unsigned> liveDecoders { 0 };
std::atomic<unsigned> liveDecoderThreads { 0 };

} // namespace

RefPtr<FFmpegVideoDecoder> FFmpegVideoDecoder::create(const FFmpegStreamInfo& stream, FFmpegPacketQueue& queue)
{
    if (!stream.parameters)
        return nullptr;
    const AVCodec* codec = findSoftwareDecoder(stream.parameters->codec_id);
    if (!codec)
        return nullptr;
    AVCodecContextPtr context(avcodec_alloc_context3(codec));
    if (!context || avcodec_parameters_to_context(context.get(), stream.parameters.get()) < 0)
        return nullptr;
    context->pkt_timebase = stream.timeBase;
    context->thread_count = decoderThreads();
    context->thread_type = frameThreadsEnabled() ? (FF_THREAD_FRAME | FF_THREAD_SLICE) : FF_THREAD_SLICE;
    int result = avcodec_open2(context.get(), codec, nullptr);
    if (result < 0) {
        ffmpegMediaLog("video decoder %s: open failed: %s", codec->name, ffmpegErrorString(result).utf8().legacyCStringPointer());
        return nullptr;
    }
    ffmpegMediaLog("video decoder %s: %dx%d %s, %d threads (%s)", codec->name, context->width, context->height,
        av_get_pix_fmt_name(context->pix_fmt) ? av_get_pix_fmt_name(context->pix_fmt) : "?", context->thread_count,
        (context->active_thread_type & FF_THREAD_FRAME) ? "frame+slice" : (context->active_thread_type & FF_THREAD_SLICE) ? "slice" : "none");
    return adoptRef(*new FFmpegVideoDecoder(stream, queue, WTF::move(context)));
}

FFmpegVideoDecoder::FFmpegVideoDecoder(const FFmpegStreamInfo& stream, FFmpegPacketQueue& queue, AVCodecContextPtr&& context)
    : m_timeBase(stream.timeBase)
    , m_startTime(stream.startTime)
    , m_queue(queue)
    , m_codec(WTF::move(context))
    , m_frame(av_frame_alloc())
{
    updateOutputSize(m_codec->width, m_codec->height);
    ++liveDecoders;
    liveDecoderThreads += static_cast<unsigned>(std::max(1, m_codec->thread_count));
}

FFmpegVideoDecoder::~FFmpegVideoDecoder()
{
    ASSERT(!m_thread);
    --liveDecoders;
    liveDecoderThreads -= static_cast<unsigned>(std::max(1, m_codec->thread_count));
    if (m_sws)
        sws_freeContext(m_sws);
}

void FFmpegVideoDecoder::start()
{
    if (m_thread || !m_frame)
        return;
    m_thread = Thread::create("FFmpegVideoDecoder"_s, [protectedThis = Ref { *this }] {
        protectedThis->run();
    });
}

void FFmpegVideoDecoder::stop()
{
    m_stopping = true;
    {
        Locker locker { m_lock };
        m_condition.notifyAll();
    }
    if (RefPtr thread = std::exchange(m_thread, nullptr))
        thread->waitForCompletion();
}

void FFmpegVideoDecoder::updateOutputSize(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;
    AVRational sar = m_codec->sample_aspect_ratio;
    double displayWidth = width;
    if (sar.num > 0 && sar.den > 0)
        displayWidth = width * av_q2d(sar);
    double scale = std::min({ 1.0, maxOutputWidth / displayWidth, maxOutputHeight / static_cast<double>(height) });
    // Even dimensions keep swscale on its fast paths.
    int outputWidth = std::max(2, static_cast<int>(std::lround(displayWidth * scale)) & ~1);
    int outputHeight = std::max(2, static_cast<int>(std::lround(height * scale)) & ~1);
    m_outputWidth = outputWidth;
    m_outputHeight = outputHeight;
}

void FFmpegVideoDecoder::run()
{
    while (!m_stopping) {
        auto entry = m_queue.pop(packetWaitTimeout);
        if (!entry) {
            if (m_queue.isAborted())
                break;
            continue;
        }
        if (entry->serial != m_serial)
            flushForSerial(entry->serial);
        if (entry->isEndOfStream()) {
            avcodec_send_packet(m_codec.get(), nullptr);
            receiveFrames();
            // Draining leaves the codec in EOF state; reset it for whatever
            // comes next (a seek, or a looping restart).
            avcodec_flush_buffers(m_codec.get());
            Locker locker { m_lock };
            m_endSerial = m_serial;
            m_condition.notifyAll();
            continue;
        }
        if (!handlePacket(entry->packet.get()))
            break;
    }
}

void FFmpegVideoDecoder::flushForSerial(unsigned serial)
{
    avcodec_flush_buffers(m_codec.get());
    m_serial = serial;
    m_waitingForKeyframe = false;
    m_lateFrames = 0;
    if (m_skippingLoopFilter) {
        m_codec->skip_loop_filter = AVDISCARD_DEFAULT;
        m_skippingLoopFilter = false;
    }
    Locker locker { m_lock };
    m_output.removeAllMatching([serial](auto& frame) {
        return frame->serial != serial;
    });
    m_endSerial = std::numeric_limits<unsigned>::max();
    m_condition.notifyAll();
}

bool FFmpegVideoDecoder::handlePacket(AVPacket* packet)
{
    if (m_suspended) {
        m_waitingForKeyframe = true;
        return true;
    }
    if (m_waitingForKeyframe) {
        if (!(packet->flags & AV_PKT_FLAG_KEY))
            return true;
        avcodec_flush_buffers(m_codec.get());
        m_waitingForKeyframe = false;
    }

    int64_t start = nowMicroseconds();
    m_pendingConvertMicros = 0;
    int result = avcodec_send_packet(m_codec.get(), packet);
    if (result == AVERROR(EAGAIN)) {
        receiveFrames();
        result = avcodec_send_packet(m_codec.get(), packet);
    }
    if (result < 0 && result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
        // Corrupt packets are routine in live streams; keep going.
        if (!(m_sendErrors++ % 100))
            ffmpegMediaLog("video decoder: send_packet: %s (%u errors)", ffmpegErrorString(result).utf8().legacyCStringPointer(), m_sendErrors);
    }
    receiveFrames();
    // Wall time in libavcodec (with frame threads: waiting for the
    // threads), without the conversions done in between.
    m_decodeMicros += std::max<int64_t>(0, nowMicroseconds() - start - m_pendingConvertMicros);
    return !m_stopping;
}

FFmpegVideoDecoder::Timing FFmpegVideoDecoder::takeTiming()
{
    unsigned decoded = m_decodedFrames;
    unsigned converted = m_convertedFrames;
    int64_t decodeMicros = m_decodeMicros;
    int64_t convertMicros = m_convertMicros;
    Timing timing;
    if (decoded > m_timingDecoded)
        timing.decodeMilliseconds = (decodeMicros - m_timingDecodeMicros) / 1000.0 / (decoded - m_timingDecoded);
    if (converted > m_timingConverted)
        timing.convertMilliseconds = (convertMicros - m_timingConvertMicros) / 1000.0 / (converted - m_timingConverted);
    m_timingDecoded = decoded;
    m_timingConverted = converted;
    m_timingDecodeMicros = decodeMicros;
    m_timingConvertMicros = convertMicros;
    return timing;
}

void FFmpegVideoDecoder::receiveFrames()
{
    while (!m_stopping) {
        int result = avcodec_receive_frame(m_codec.get(), m_frame.get());
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            return;
        if (result < 0) {
            if (!(m_sendErrors++ % 100))
                ffmpegMediaLog("video decoder: receive_frame: %s", ffmpegErrorString(result).utf8().legacyCStringPointer());
            return;
        }
        ++m_decodedFrames;
        emitFrame(m_frame.get());
        av_frame_unref(m_frame.get());
    }
}

RefPtr<FFmpegVideoFrame> FFmpegVideoDecoder::takePoolFrame(int width, int height)
{
    // A pooled frame referenced only by the pool is neither queued nor held
    // by the player or a NativeImage, so it can be overwritten.
    m_pool.removeAllMatching([&](auto& frame) {
        return frame->hasOneRef() && (frame->width() != width || frame->height() != height);
    });
    for (auto& frame : m_pool) {
        if (frame->hasOneRef())
            return frame.ptr();
    }
    RefPtr frame = FFmpegVideoFrame::create(width, height);
    if (!frame) {
        ffmpegMediaLog("video decoder: cannot allocate a %dx%d frame", width, height);
        return nullptr;
    }
    if (m_pool.size() < maxPooledFrames) {
        m_pool.append(Ref { *frame });
        ffmpegMediaLog("video decoder: frame pool %zu x %dx%d (stride %zu, %zu KiB each)", m_pool.size(), width, height,
            frame->stride(), frame->sizeInBytes() >> 10);
    }
    return frame;
}

void FFmpegVideoDecoder::emitFrame(AVFrame* frame)
{
    double pts = (frame->best_effort_timestamp == AV_NOPTS_VALUE || !m_timeBase.den)
        ? std::numeric_limits<double>::quiet_NaN()
        : frame->best_effort_timestamp * av_q2d(m_timeBase) - m_startTime;
    double duration = frame->duration > 0 ? frame->duration * av_q2d(m_timeBase) : 0;
    if (duration <= 0 && m_codec->framerate.num > 0 && m_codec->framerate.den > 0)
        duration = av_q2d(av_inv_q(m_codec->framerate));
    if (duration <= 0)
        duration = 1.0 / 30;
    if (!std::isfinite(pts))
        return;

    {
        Locker locker { m_lock };
        if (m_seekTarget >= 0 && m_seekSerial == m_serial) {
            if (pts + duration <= m_seekTarget)
                return;
            m_seekTarget = -1;
        }
    }

    bool isLate = pts + duration < m_clockHint - lateThreshold;
    if (isLate) {
        ++m_lateFrames;
        if (m_lateFrames >= lateFramesBeforeSkippingLoopFilter && !m_skippingLoopFilter) {
            // Deblocking is a large share of H.264 decode time; skipping it
            // costs some quality but lets a slow CPU catch up.
            m_codec->skip_loop_filter = AVDISCARD_NONREF;
            m_skippingLoopFilter = true;
        }
        if (m_lateFrames >= lateFramesBeforeSkippingAllLoopFilter && m_codec->skip_loop_filter != AVDISCARD_ALL)
            m_codec->skip_loop_filter = AVDISCARD_ALL;
        if (m_lateFrames % lateFrameKeepInterval) {
            ++m_droppedFrames;
            return;
        }
    } else {
        m_lateFrames = 0;
        if (m_skippingLoopFilter) {
            m_codec->skip_loop_filter = AVDISCARD_DEFAULT;
            m_skippingLoopFilter = false;
        }
    }

    updateOutputSize(frame->width, frame->height);
    int width = m_outputWidth;
    int height = m_outputHeight;
    RefPtr output = takePoolFrame(width, height);
    if (!output) {
        ++m_droppedFrames;
        return;
    }
    // Same size: only the YUV to BGRA conversion, no filtering needed.
    int64_t convertStart = nowMicroseconds();
    int swsFlags = (width == frame->width && height == frame->height) ? SWS_POINT : SWS_FAST_BILINEAR;
    m_sws = sws_getCachedContext(m_sws, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
        width, height, AV_PIX_FMT_BGRA, swsFlags, nullptr, nullptr, nullptr);
    if (!m_sws) {
        ffmpegMediaLog("video decoder: no swscale path from %s", av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)));
        m_fatalError = true;
        return;
    }
    bool fullRange = frame->color_range == AVCOL_RANGE_JPEG;
    sws_setColorspaceDetails(m_sws, sws_getCoefficients(swsColorspace(frame)), fullRange ? 1 : 0,
        sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
    uint8_t* destination[4] = { output->data(), nullptr, nullptr, nullptr };
    int destinationStride[4] = { static_cast<int>(output->stride()), 0, 0, 0 };
    sws_scale(m_sws, frame->data, frame->linesize, 0, frame->height, destination, destinationStride);
    int64_t convertMicros = nowMicroseconds() - convertStart;
    m_convertMicros += convertMicros;
    m_pendingConvertMicros += convertMicros;
    ++m_convertedFrames;
    output->pts = pts;
    output->duration = duration;
    output->serial = m_serial;

    Locker locker { m_lock };
    while (m_output.size() >= maxOutputFrames && !m_stopping && m_queue.serial() == m_serial)
        m_condition.waitFor(m_lock, outputWaitTimeout);
    if (m_stopping || m_queue.serial() != m_serial)
        return;
    m_output.append(output.releaseNonNull());
}

RefPtr<FFmpegVideoFrame> FFmpegVideoDecoder::peekFrame(unsigned serial, size_t index)
{
    Locker locker { m_lock };
    while (!m_output.isEmpty() && m_output.first()->serial != serial) {
        m_output.removeFirst();
        m_condition.notifyAll();
    }
    if (index >= m_output.size())
        return nullptr;
    auto it = m_output.begin();
    for (size_t i = 0; i < index; ++i)
        ++it;
    if ((*it)->serial != serial)
        return nullptr;
    return it->ptr();
}

void FFmpegVideoDecoder::popFrame()
{
    Locker locker { m_lock };
    if (!m_output.isEmpty())
        m_output.removeFirst();
    m_condition.notifyAll();
}

size_t FFmpegVideoDecoder::queuedFrames() const
{
    Locker locker { m_lock };
    return m_output.size();
}

bool FFmpegVideoDecoder::reachedEnd(unsigned serial) const
{
    Locker locker { m_lock };
    return m_endSerial == serial && m_output.isEmpty();
}

void FFmpegVideoDecoder::setSeekTarget(double time, unsigned serial)
{
    m_clockHint = time;
    Locker locker { m_lock };
    m_seekTarget = time;
    m_seekSerial = serial;
    m_condition.notifyAll();
}

void FFmpegVideoDecoder::setSuspended(bool suspended)
{
    m_suspended = suspended;
}

} // namespace WebCore

// For the shell's CPU monitor (src/frontend/ps5/ps5_cpu_monitor.cpp, which
// declares it weak): live video decoders and their libavcodec threads.
extern "C" void ps5_media_video_decoder_stats(unsigned* decoders, unsigned* threads);
extern "C" void ps5_media_video_decoder_stats(unsigned* decoders, unsigned* threads)
{
    *decoders = WebCore::liveDecoders.load();
    *threads = WebCore::liveDecoderThreads.load();
}

#endif // ENABLE(VIDEO) && USE(FFMPEG)
