// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "FFmpegVideoDecoder.h"

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "FFmpegDemuxer.h"
#include "FFmpegLog.h"
#include "FFmpegVideoOverlay.h"
#include "FFmpegVideoOverlayClient.h"
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
// networking and the audio decoder (PROSPEROWEB_MEDIA_THREADS=1..16).
// Frame and slice threading: frame threading is what scales H.264 and VP9
// across cores. The console crashes of run 4/5 that first pointed at the
// frame threads ("av:h264:df*") were the swscale overrun fixed in
// FFmpegPlaybackPolicy.h, not frame threading;
// PROSPEROWEB_MEDIA_FRAME_THREADS=0 goes back to slice threads only.
constexpr int defaultDecoderThreads = 6;

int decoderThreads()
{
    if (const char* value = getenv("PROSPEROWEB_MEDIA_THREADS")) {
        int count = atoi(value);
        if (count >= 1 && count <= 16)
            return count;
    }
    return defaultDecoderThreads;
}

bool frameThreadsEnabled()
{
    const char* value = getenv("PROSPEROWEB_MEDIA_FRAME_THREADS");
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

namespace Wire = FFmpegMedia::VideoWire;

// Who decodes when the player has a video overlay: "ui" asks the UI process
// first (hardware), "webprocess" never does.
bool remoteDecodingRequested()
{
    const char* value = getenv("PROSPEROWEB_VIDEO_DECODE");
    if (value && *value)
        return !strcmp(value, "ui");
#if PLATFORM(PLAYSTATION)
    return true;
#else
    return false;
#endif
}

// The UI answers once its decoder exists (memory allocation included).
constexpr double remoteAnswerTimeoutSeconds = 3;
constexpr double remoteEventWaitSeconds = 0.005;
constexpr Seconds remotePacketWait = 5_ms;

std::optional<Wire::StreamOpen> remoteStreamOpen(const FFmpegStreamInfo& stream)
{
    const AVCodecParameters* parameters = stream.parameters.get();
    Wire::StreamOpen open;
    switch (parameters->codec_id) {
    case AV_CODEC_ID_H264:
        open.codec = Wire::Codec::H264;
        break;
    case AV_CODEC_ID_HEVC:
        open.codec = Wire::Codec::Hevc;
        break;
    case AV_CODEC_ID_VP9:
        open.codec = Wire::Codec::Vp9;
        break;
    case AV_CODEC_ID_AV1:
        open.codec = Wire::Codec::Av1;
        break;
    default:
        return std::nullopt;
    }
    switch (parameters->color_space) {
    case AVCOL_SPC_BT709:
        open.matrix = Wire::Matrix::Bt709;
        break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        open.matrix = Wire::Matrix::Bt2020;
        break;
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_BT470BG:
        open.matrix = Wire::Matrix::Bt601;
        break;
    default:
        break;
    }
    open.fullRange = parameters->color_range == AVCOL_RANGE_JPEG;
    open.profile = parameters->profile;
    open.level = parameters->level;
    open.width = static_cast<uint32_t>(std::max(0, parameters->width));
    open.height = static_cast<uint32_t>(std::max(0, parameters->height));
    open.timeBaseNum = stream.timeBase.num > 0 ? stream.timeBase.num : 1;
    open.timeBaseDen = stream.timeBase.den > 0 ? stream.timeBase.den : 1000000;
    if (parameters->extradata && parameters->extradata_size > 0)
        open.extradata.assign(parameters->extradata, parameters->extradata + parameters->extradata_size);
    return open;
}

// Process-wide totals for ps5_media_video_decoder_stats().
std::atomic<unsigned> liveDecoders { 0 };
std::atomic<unsigned> liveDecoderThreads { 0 };

} // namespace

RefPtr<FFmpegVideoDecoder> FFmpegVideoDecoder::create(const FFmpegStreamInfo& stream, FFmpegPacketQueue& queue, FFmpegVideoOverlay* overlay)
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

    // Remote decoding: the UI is asked now (the answer arrives while the
    // player sets up); libavcodec opens only if it refuses.
    std::shared_ptr<Wire::RemoteVideoStream> remote;
    std::optional<Wire::StreamOpen> open;
    if (overlay && remoteDecodingRequested()) {
        open = remoteStreamOpen(stream);
        if (open)
            remote = overlay->openRemoteStream(*open);
    }
    if (remote) {
        ffmpegMediaLog("video decoder %s: %dx%d, asking the UI to decode it (stream %u)", codec->name, context->width, context->height, remote->id());
        Ref decoder = adoptRef(*new FFmpegVideoDecoder(stream, queue, WTF::move(context), false));
        decoder->m_remote = WTF::move(remote);
        decoder->m_remoteOpen = std::make_unique<Wire::StreamOpen>(WTF::move(*open));
        return decoder;
    }

    int result = avcodec_open2(context.get(), codec, nullptr);
    if (result < 0) {
        ffmpegMediaLog("video decoder %s: open failed: %s", codec->name, ffmpegErrorString(result).utf8().legacyCStringPointer());
        return nullptr;
    }
    ffmpegMediaLog("video decoder %s: %dx%d %s, %d threads (%s)", codec->name, context->width, context->height,
        av_get_pix_fmt_name(context->pix_fmt) ? av_get_pix_fmt_name(context->pix_fmt) : "?", context->thread_count,
        (context->active_thread_type & FF_THREAD_FRAME) ? "frame+slice" : (context->active_thread_type & FF_THREAD_SLICE) ? "slice" : "none");
    return adoptRef(*new FFmpegVideoDecoder(stream, queue, WTF::move(context), true));
}

bool FFmpegVideoDecoder::openSoftwareDecoder()
{
    if (m_codecOpen)
        return true;
    const AVCodec* codec = m_codec->codec;
    int result = avcodec_open2(m_codec.get(), codec, nullptr);
    if (result < 0) {
        ffmpegMediaLog("video decoder %s: open failed: %s", codec ? codec->name : "?", ffmpegErrorString(result).utf8().legacyCStringPointer());
        m_fatalError = true;
        return false;
    }
    m_codecOpen = true;
    ffmpegMediaLog("video decoder %s: decoding here: %dx%d %s, %d threads (%s)", codec ? codec->name : "?", m_codec->width, m_codec->height,
        av_get_pix_fmt_name(m_codec->pix_fmt) ? av_get_pix_fmt_name(m_codec->pix_fmt) : "?", m_codec->thread_count,
        (m_codec->active_thread_type & FF_THREAD_FRAME) ? "frame+slice" : (m_codec->active_thread_type & FF_THREAD_SLICE) ? "slice" : "none");
    return true;
}

FFmpegVideoDecoder::FFmpegVideoDecoder(const FFmpegStreamInfo& stream, FFmpegPacketQueue& queue, AVCodecContextPtr&& context, bool codecOpen)
    : m_timeBase(stream.timeBase)
    , m_codecOpen(codecOpen)
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
    // The UI's decoder and its pictures go now (MSE removes a SourceBuffer
    // while the overlay stays).
    if (m_remote) {
        m_remoteActive = false;
        m_remote->close("the player stopped");
    }
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
    if (m_remote) {
        runRemote();
        if (m_stopping || !openSoftwareDecoder())
            return;
    }
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
    if (m_codecOpen)
        avcodec_flush_buffers(m_codec.get());
    m_serial = serial;
    if (m_remoteActive) {
        m_remote->flush(serial);
        m_remoteDurations.clear();
    }
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

void FFmpegVideoDecoder::runRemote()
{
    auto remote = m_remote;
    auto state = remote->waitForAnswer(remoteAnswerTimeoutSeconds, m_stopping);
    if (state != Wire::RemoteVideoStream::State::Hardware) {
        ffmpegMediaLog("video decoder: the UI does not decode stream %u (%s: %s), decoding in the WebProcess", remote->id(),
            Wire::RemoteVideoStream::stateName(state), remote->reason().c_str());
        return;
    }
    ffmpegMediaLog("video decoder: stream %u decoded by the UI (hardware, %u frames held there)", remote->id(), remote->maxFrames());
    m_remoteActive = true;
    std::optional<FFmpegPacketQueue::Entry> pending;
    bool suspended = false;
    while (!m_stopping) {
        processRemoteNotices();
        if (!remote->usable()) {
            endRemote(remote->reason().c_str());
            // Software from the next keyframe; this packet may be one.
            m_waitingForKeyframe = true;
            if (!openSoftwareDecoder())
                return;
            if (pending && pending->serial == m_serial && !pending->isEndOfStream())
                handlePacket(pending->packet.get());
            return;
        }
        // A seek (the queue moved to a new serial): flush the UI's decoder
        // now. Waiting for the first packet of the new position deadlocks:
        // a packet of the old position may be waiting here for credit that
        // only comes back when the UI's decoder gets a picture slot, and its
        // slots hold old pictures the presenter will never show.
        if (unsigned queueSerial = m_queue.serial(); queueSerial != m_serial) {
            if (pending && pending->serial != queueSerial)
                pending.reset();
            flushForSerial(queueSerial);
        }
        if (!pending) {
            pending = m_queue.pop(remotePacketWait);
            if (!pending) {
                if (m_queue.isAborted())
                    return;
                continue;
            }
        }
        if (pending->serial != m_serial)
            flushForSerial(pending->serial);
        if (pending->isEndOfStream()) {
            remote->drain(m_serial);
            pending.reset();
            continue;
        }
        AVPacket* packet = pending->packet.get();
        if (m_suspended) {
            if (!std::exchange(suspended, true))
                ffmpegMediaLog("video decoder: stream %u suspended (page hidden) at clock %.3f; holding its input queue", remote->id(), m_clockHint.load());
            m_waitingForKeyframe = true;
            // Do NOT drain the queue while the page is hidden. Draining keeps
            // pulling the source's enqueue cursor (MSE) or the demuxer's read
            // position (progressive) forward to the end of the buffered
            // range; decode would then resume far AHEAD of the audio clock,
            // and the player, whose clock only reaches those pts seconds or
            // minutes later, would show nothing while the UI's hardware
            // decoder fills every picture slot with frames it never shows
            // (the tab-switch freeze). Holding `pending` fills the input
            // queue instead, so the source stops feeding and the cursor stays
            // at the clock; on resume decode restarts there, behind the clock
            // that advanced meanwhile, and the drop-behind below catches up.
            remote->waitForEvent(remoteEventWaitSeconds);
            continue;
        }
        if (m_waitingForKeyframe) {
            if (!(packet->flags & AV_PKT_FLAG_KEY)) {
                pending.reset();
                continue;
            }
            // Resuming after a hidden page: the UI's decoder starts over, and
            // the pictures before the clock are dropped as they come out (as
            // after a seek) so the next one due is shown, not a stale one.
            remote->flush(m_serial);
            m_remoteDurations.clear();
            m_waitingForKeyframe = false;
            if (std::exchange(suspended, false)) {
                double clock = m_clockHint.load();
                {
                    Locker locker { m_lock };
                    if (m_seekTarget < 0 || m_seekSerial != m_serial) {
                        m_seekTarget = clock;
                        m_seekSerial = m_serial;
                    }
                }
                ffmpegMediaLog("video decoder: stream %u resumed (page shown) at keyframe pts %.3f, clock %.3f", remote->id(),
                    packet->pts != AV_NOPTS_VALUE ? packet->pts * av_q2d(m_timeBase) - m_startTime : -1.0, clock);
            }
        }
        if (!remote->hasCredit(static_cast<size_t>(std::max(0, packet->size)))) {
            if (m_queue.serial() != pending->serial)
                pending.reset(); // a seek meanwhile: flushed at the top
            remote->waitForEvent(remoteEventWaitSeconds);
            continue;
        }
        sendRemotePacket(packet);
        pending.reset();
    }
}

void FFmpegVideoDecoder::endRemote(const char* why)
{
    if (!m_remoteActive.exchange(false))
        return;
    ffmpegMediaLog("video decoder: the UI stopped decoding stream %u (%s), decoding in the WebProcess from the next keyframe", m_remote->id(), why);
    m_remote->close(why);
    m_remoteDurations.clear();
    // Frames the UI holds cannot be shown any more.
    Locker locker { m_lock };
    m_output.removeAllMatching([](auto& frame) {
        return frame->isRemote();
    });
    m_condition.notifyAll();
}

void FFmpegVideoDecoder::sendRemotePacket(AVPacket* packet)
{
    size_t newExtradataSize = 0;
    if (const uint8_t* extradata = av_packet_get_side_data(packet, AV_PKT_DATA_NEW_EXTRADATA, &newExtradataSize); extradata && newExtradataSize) {
        m_remoteOpen->extradata.assign(extradata, extradata + newExtradataSize);
        m_remote->reconfigure(*m_remoteOpen);
    }
    int64_t pts = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
    if (pts == AV_NOPTS_VALUE) {
        ++m_droppedFrames;
        return;
    }
    if (packet->duration > 0) {
        m_remoteDurations[pts] = packet->duration;
        while (m_remoteDurations.size() > 512)
            m_remoteDurations.erase(m_remoteDurations.begin());
    }
    Wire::PacketHeader header;
    header.serial = m_serial;
    header.pts = pts;
    header.dts = packet->dts != AV_NOPTS_VALUE ? packet->dts : pts;
    header.keyframe = packet->flags & AV_PKT_FLAG_KEY;
    m_remote->sendPacket(header, packet->data, static_cast<size_t>(std::max(0, packet->size)));
}

void FFmpegVideoDecoder::processRemoteNotices()
{
    for (auto& notice : m_remote->takeNotices()) {
        if (notice.serial != m_serial)
            continue;
        if (notice.drained()) {
            Locker locker { m_lock };
            m_endSerial = m_serial;
            m_condition.notifyAll();
            continue;
        }
        ++m_decodedFrames;
        emitRemoteFrame(notice.pts, static_cast<int>(notice.width), static_cast<int>(notice.height));
    }
}

void FFmpegVideoDecoder::emitRemoteFrame(int64_t remotePts, int width, int height)
{
    double pts = remotePts * av_q2d(m_timeBase) - m_startTime;
    double duration = 0;
    if (auto it = m_remoteDurations.find(remotePts); it != m_remoteDurations.end())
        duration = it->second * av_q2d(m_timeBase);
    m_remoteDurations.erase(m_remoteDurations.begin(), m_remoteDurations.upper_bound(remotePts));
    if (duration <= 0 && m_codec->framerate.num > 0 && m_codec->framerate.den > 0)
        duration = av_q2d(av_inv_q(m_codec->framerate));
    if (duration <= 0)
        duration = 1.0 / 30;
    {
        Locker locker { m_lock };
        if (m_seekTarget >= 0 && m_seekSerial == m_serial) {
            if (pts + duration <= m_seekTarget) {
                m_remote->discard(m_serial, remotePts);
                return;
            }
            m_seekTarget = -1;
        }
    }
    updateOutputSize(width, height);
    Ref frame = FFmpegVideoFrame::createRemote(m_outputWidth, m_outputHeight, m_remote->id(), remotePts);
    frame->pts = pts;
    frame->duration = duration;
    frame->serial = m_serial;

    Locker locker { m_lock };
    while (m_output.size() >= maxOutputFrames && !m_stopping && m_queue.serial() == m_serial)
        m_condition.waitFor(m_lock, outputWaitTimeout);
    if (m_stopping || m_queue.serial() != m_serial)
        return;
    m_output.append(WTF::move(frame));
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
