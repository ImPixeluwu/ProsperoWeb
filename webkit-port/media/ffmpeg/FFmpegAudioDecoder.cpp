// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "FFmpegAudioDecoder.h"

#if ENABLE(VIDEO) && USE(FFMPEG)

#include "FFmpegAVClock.h"
#include "FFmpegDemuxer.h"
#include "Logging.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace WebCore {

namespace {

constexpr Seconds packetWaitTimeout = 20_ms;
constexpr Seconds spaceWaitTimeout = 10_ms;
// Decoded audio kept ahead of the sink.
constexpr double ringSeconds = 1.0;
// Timestamp gaps larger than this are filled with silence (up to the cap)
// so the audio clock stays linear.
constexpr double gapThreshold = 0.04;
constexpr double maxGapFill = 2.0;
// Discard mode keeps the decoder at most this far ahead of the clock.
constexpr double discardLead = 0.2;

} // namespace

RefPtr<FFmpegAudioDecoder> FFmpegAudioDecoder::create(const FFmpegStreamInfo& stream, FFmpegPacketQueue& queue, int outputRate, int outputChannels)
{
    if (!stream.parameters || outputRate <= 0 || outputChannels <= 0)
        return nullptr;
    const AVCodec* codec = findSoftwareDecoder(stream.parameters->codec_id);
    if (!codec)
        return nullptr;
    AVCodecContextPtr context(avcodec_alloc_context3(codec));
    if (!context || avcodec_parameters_to_context(context.get(), stream.parameters.get()) < 0)
        return nullptr;
    context->pkt_timebase = stream.timeBase;
    context->thread_count = 1;
    int result = avcodec_open2(context.get(), codec, nullptr);
    if (result < 0) {
        LOG(Media, "FFmpegAudioDecoder: cannot open %s: %s", codec->name, ffmpegErrorString(result).utf8().legacyCStringPointer());
        return nullptr;
    }
    return adoptRef(*new FFmpegAudioDecoder(stream, queue, WTF::move(context), outputRate, outputChannels));
}

FFmpegAudioDecoder::FFmpegAudioDecoder(const FFmpegStreamInfo& stream, FFmpegPacketQueue& queue, AVCodecContextPtr&& context, int outputRate, int outputChannels)
    : m_timeBase(stream.timeBase)
    , m_startTime(stream.startTime)
    , m_outputRate(outputRate)
    , m_outputChannels(outputChannels)
    , m_queue(queue)
    , m_codec(WTF::move(context))
    , m_frame(av_frame_alloc())
    , m_ring(static_cast<size_t>(outputRate * ringSeconds) * outputChannels)
{
}

FFmpegAudioDecoder::~FFmpegAudioDecoder()
{
    ASSERT(!m_thread);
    swr_free(&m_swr);
    av_channel_layout_uninit(&m_swrInputLayout);
}

void FFmpegAudioDecoder::start()
{
    if (m_thread || !m_frame)
        return;
    m_thread = Thread::create("FFmpegAudioDecoder"_s, [protectedThis = Ref { *this }] {
        protectedThis->run();
    });
}

void FFmpegAudioDecoder::stop()
{
    m_stopping = true;
    {
        Locker locker { m_lock };
        m_condition.notifyAll();
    }
    if (RefPtr thread = std::exchange(m_thread, nullptr))
        thread->waitForCompletion();
}

void FFmpegAudioDecoder::run()
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
            avcodec_flush_buffers(m_codec.get());
            Locker locker { m_lock };
            m_endSerial = m_serial;
            continue;
        }
        decodePacket(entry->packet.get());
    }
}

void FFmpegAudioDecoder::flushForSerial(unsigned serial)
{
    avcodec_flush_buffers(m_codec.get());
    // Recreated on the next frame, dropping samples buffered inside swr.
    swr_free(&m_swr);
    m_swrInputRate = 0;
    m_serial = serial;
    Locker locker { m_lock };
    m_ring.clear();
    m_ringSerial = serial;
    m_basePts.reset();
    m_framesWritten = 0;
    m_framesConsumed = 0;
    m_endSerial = std::numeric_limits<unsigned>::max();
    m_condition.notifyAll();
}

void FFmpegAudioDecoder::decodePacket(AVPacket* packet)
{
    int result = avcodec_send_packet(m_codec.get(), packet);
    if (result == AVERROR(EAGAIN)) {
        receiveFrames();
        result = avcodec_send_packet(m_codec.get(), packet);
    }
    if (result < 0 && result != AVERROR(EAGAIN) && result != AVERROR_EOF)
        LOG(Media, "FFmpegAudioDecoder: send_packet: %s", ffmpegErrorString(result).utf8().legacyCStringPointer());
    receiveFrames();
}

void FFmpegAudioDecoder::receiveFrames()
{
    while (!m_stopping) {
        int result = avcodec_receive_frame(m_codec.get(), m_frame.get());
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            return;
        if (result < 0) {
            LOG(Media, "FFmpegAudioDecoder: receive_frame: %s", ffmpegErrorString(result).utf8().legacyCStringPointer());
            return;
        }
        writeFrame(m_frame.get());
        av_frame_unref(m_frame.get());
    }
}

bool FFmpegAudioDecoder::ensureResampler(const AVFrame* frame)
{
    if (m_swr && frame->sample_rate == m_swrInputRate && frame->format == m_swrInputFormat
        && !av_channel_layout_compare(&frame->ch_layout, &m_swrInputLayout))
        return true;

    swr_free(&m_swr);
    AVChannelLayout input { };
    if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || av_channel_layout_check(&frame->ch_layout) != 1)
        av_channel_layout_default(&input, std::max(frame->ch_layout.nb_channels, 1));
    else if (av_channel_layout_copy(&input, &frame->ch_layout) < 0)
        return false;
    AVChannelLayout output { };
    av_channel_layout_default(&output, m_outputChannels);
    int result = swr_alloc_set_opts2(&m_swr, &output, AV_SAMPLE_FMT_FLT, m_outputRate,
        &input, static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr);
    av_channel_layout_uninit(&output);
    av_channel_layout_uninit(&input);
    if (result < 0 || !m_swr || swr_init(m_swr) < 0) {
        LOG(Media, "FFmpegAudioDecoder: cannot resample %d Hz format %d", frame->sample_rate, frame->format);
        swr_free(&m_swr);
        m_fatalError = true;
        return false;
    }
    m_swrInputRate = frame->sample_rate;
    m_swrInputFormat = frame->format;
    av_channel_layout_uninit(&m_swrInputLayout);
    av_channel_layout_copy(&m_swrInputLayout, &frame->ch_layout);
    return true;
}

void FFmpegAudioDecoder::writeFrame(const AVFrame* frame)
{
    if (frame->nb_samples <= 0 || frame->sample_rate <= 0 || !ensureResampler(frame))
        return;
    double pts = (frame->best_effort_timestamp == AV_NOPTS_VALUE || !m_timeBase.den)
        ? std::numeric_limits<double>::quiet_NaN()
        : frame->best_effort_timestamp * av_q2d(m_timeBase) - m_startTime;

    int capacity = swr_get_out_samples(m_swr, frame->nb_samples);
    if (capacity <= 0)
        return;
    m_converted.resize(static_cast<size_t>(capacity) * m_outputChannels);
    uint8_t* output = reinterpret_cast<uint8_t*>(m_converted.mutableSpan().data());
    int converted = swr_convert(m_swr, &output, capacity, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
    if (converted <= 0)
        return;
    writeSamples(m_converted.span().first(static_cast<size_t>(converted) * m_outputChannels), pts);
}

bool FFmpegAudioDecoder::waitForSpace()
{
    while (!m_ring.freeSpace()) {
        if (m_stopping || m_discard || m_queue.serial() != m_serial)
            return false;
        m_condition.waitFor(m_lock, spaceWaitTimeout);
    }
    return !m_stopping && m_queue.serial() == m_serial;
}

void FFmpegAudioDecoder::writeSamples(std::span<const float> samples, double pts)
{
    const size_t channels = static_cast<size_t>(m_outputChannels);
    if (m_discard) {
        // Pace the decoder against the clock, then drop.
        while (m_discard && !m_stopping && m_queue.serial() == m_serial && std::isfinite(pts) && pts > m_clockHint + discardLead) {
            Locker locker { m_lock };
            m_condition.waitFor(m_lock, spaceWaitTimeout);
        }
        m_needsResync = true;
        return;
    }

    Locker locker { m_lock };
    if (m_needsResync.exchange(false)) {
        m_ring.clear();
        m_basePts.reset();
        m_framesWritten = 0;
        m_framesConsumed = 0;
    }

    if (m_seekTarget >= 0 && m_seekSerial == m_serial && std::isfinite(pts)) {
        double end = pts + static_cast<double>(samples.size() / channels) / m_outputRate;
        if (end <= m_seekTarget)
            return;
        if (pts < m_seekTarget) {
            size_t skip = static_cast<size_t>((m_seekTarget - pts) * m_outputRate) * channels;
            samples = samples.subspan(std::min(skip, samples.size()));
            pts = m_seekTarget;
        }
        m_seekTarget = -1;
    }

    if (!m_basePts)
        m_basePts = std::isfinite(pts) ? pts : 0;
    else if (std::isfinite(pts)) {
        double expected = *m_basePts + static_cast<double>(m_framesWritten) / m_outputRate;
        double gap = pts - expected;
        if (gap > gapThreshold) {
            size_t silence = static_cast<size_t>(std::min(gap, maxGapFill) * m_outputRate) * channels;
            while (silence) {
                if (!waitForSpace())
                    return;
                size_t written = m_ring.fill(0.0f, std::min(silence, m_ring.freeSpace() - m_ring.freeSpace() % channels));
                if (!written)
                    continue;
                silence -= written;
                m_framesWritten += written / channels;
            }
        }
    }

    while (!samples.empty()) {
        if (!waitForSpace())
            return;
        size_t room = m_ring.freeSpace() - m_ring.freeSpace() % channels;
        if (!room) {
            m_condition.waitFor(m_lock, spaceWaitTimeout);
            continue;
        }
        size_t written = m_ring.write(samples.data(), std::min(room, samples.size()));
        samples = samples.subspan(written);
        m_framesWritten += written / channels;
    }
}

void FFmpegAudioDecoder::render(std::span<float> out)
{
    const size_t channels = static_cast<size_t>(m_outputChannels);
    size_t read = 0;
    {
        Locker locker { m_lock };
        // After a seek request, stay silent until the decoder has switched
        // the ring buffer to the new serial.
        bool current = m_ringSerial == m_seekSerial || m_seekSerial == 0;
        if (current && !m_discard) {
            read = m_ring.read(out.data(), out.size() - out.size() % channels);
            m_framesConsumed += read / channels;
            m_condition.notifyAll();
        }
    }
    float gain = m_gain;
    for (size_t i = 0; i < read; ++i)
        out[i] *= gain;
    std::fill(out.begin() + read, out.end(), 0.0f);
}

std::optional<double> FFmpegAudioDecoder::clockTime(double sinkLatency) const
{
    Locker locker { m_lock };
    if (!m_basePts || m_discard)
        return std::nullopt;
    return FFmpegMedia::audioClockTime(*m_basePts, m_framesConsumed, m_outputRate, sinkLatency);
}

double FFmpegAudioDecoder::bufferedSeconds() const
{
    Locker locker { m_lock };
    return static_cast<double>(m_ring.size() / static_cast<size_t>(m_outputChannels)) / m_outputRate;
}

bool FFmpegAudioDecoder::drained(unsigned serial) const
{
    Locker locker { m_lock };
    return m_endSerial == serial && m_ringSerial == serial && m_ring.isEmpty();
}

bool FFmpegAudioDecoder::hasData(unsigned serial) const
{
    Locker locker { m_lock };
    return m_ringSerial == serial && m_basePts && (!m_ring.isEmpty() || m_framesConsumed);
}

void FFmpegAudioDecoder::setSeekTarget(double time, unsigned serial)
{
    m_clockHint = time;
    Locker locker { m_lock };
    m_seekTarget = time;
    m_seekSerial = serial;
    m_condition.notifyAll();
}

void FFmpegAudioDecoder::setDiscard(bool discard)
{
    if (m_discard == discard)
        return;
    m_discard = discard;
    if (!discard)
        m_needsResync = true;
    Locker locker { m_lock };
    m_condition.notifyAll();
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG)
