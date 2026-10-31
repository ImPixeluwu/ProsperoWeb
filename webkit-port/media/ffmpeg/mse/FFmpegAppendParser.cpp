// SPDX-License-Identifier: GPL-3.0-or-later

#include "config.h"
#include "FFmpegAppendParser.h"

#if ENABLE(VIDEO) && USE(FFMPEG) && ENABLE(MEDIA_SOURCE)

#include "FFmpegLog.h"
#include <cmath>
#include <limits>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

namespace WebCore {

namespace {

constexpr int ioBufferSize = 32 * 1024;

const char* errorText(int error, char (&buffer)[AV_ERROR_MAX_STRING_SIZE])
{
    av_strerror(error, buffer, sizeof(buffer));
    return buffer;
}

int readPacket(void* opaque, uint8_t* buffer, int size)
{
    auto& queue = *static_cast<FFmpegMedia::ByteQueue*>(opaque);
    size_t count = queue.read(buffer, static_cast<size_t>(std::max(size, 0)));
    return count ? static_cast<int>(count) : AVERROR_EOF;
}

int64_t seekPacket(void* opaque, int64_t offset, int whence)
{
    auto& queue = *static_cast<FFmpegMedia::ByteQueue*>(opaque);
    if (whence & AVSEEK_SIZE)
        return static_cast<int64_t>(queue.size());
    whence &= ~AVSEEK_FORCE;
    auto mode = whence == SEEK_CUR ? FFmpegMedia::ByteQueue::Whence::Current
        : whence == SEEK_END ? FFmpegMedia::ByteQueue::Whence::End : FFmpegMedia::ByteQueue::Whence::Set;
    int64_t result = queue.seek(offset, mode);
    return result < 0 ? AVERROR(EINVAL) : result;
}

// An AVFormatContext reading a complete file from memory.
class MemoryDemuxer {
public:
    MemoryDemuxer(const std::vector<uint8_t>& head, const std::vector<uint8_t>* tail, bool seekable)
        : m_seekable(seekable)
    {
        m_queue.append(head.data(), head.size());
        if (tail)
            m_queue.append(tail->data(), tail->size());
    }

    ~MemoryDemuxer()
    {
        if (m_context)
            avformat_close_input(&m_context);
        if (m_io) {
            av_freep(&m_io->buffer);
            avio_context_free(&m_io);
        }
    }

    int open(const AVInputFormat* format)
    {
        auto* buffer = static_cast<uint8_t*>(av_malloc(ioBufferSize));
        if (!buffer)
            return AVERROR(ENOMEM);
        m_io = avio_alloc_context(buffer, ioBufferSize, 0, &m_queue, readPacket, nullptr, m_seekable ? seekPacket : nullptr);
        if (!m_io) {
            av_free(buffer);
            return AVERROR(ENOMEM);
        }
        m_io->seekable = m_seekable ? AVIO_SEEKABLE_NORMAL : 0;
        m_context = avformat_alloc_context();
        if (!m_context)
            return AVERROR(ENOMEM);
        m_context->pb = m_io;
        m_context->flags |= AVFMT_FLAG_CUSTOM_IO;
        // Fail rather than conceal: a broken segment is an append error.
        m_context->error_recognition = 0;
        int result = avformat_open_input(&m_context, nullptr, format, nullptr);
        // avformat_open_input() frees the context on failure.
        return result;
    }

    AVFormatContext* context() const { return m_context; }

private:
    FFmpegMedia::ByteQueue m_queue;
    AVIOContext* m_io { nullptr };
    AVFormatContext* m_context { nullptr };
    const bool m_seekable;
};

uint64_t trackIDForStream(const AVStream* stream)
{
    return stream->id > 0 ? static_cast<uint64_t>(stream->id) : static_cast<uint64_t>(stream->index) + 1;
}

} // namespace

FFmpegParsedTrack::FFmpegParsedTrack(const FFmpegParsedTrack& other)
    : trackID(other.trackID)
    , type(other.type)
    , timeBase(other.timeBase)
    , sampleAspectRatio(other.sampleAspectRatio)
    , codecName(other.codecName)
    , fourCC(other.fourCC)
{
    if (other.parameters) {
        parameters = avcodec_parameters_alloc();
        if (parameters && avcodec_parameters_copy(parameters, other.parameters) < 0)
            avcodec_parameters_free(&parameters);
    }
}

FFmpegParsedTrack::~FFmpegParsedTrack()
{
    if (parameters)
        avcodec_parameters_free(&parameters);
}

FFmpegAppendParser::FFmpegAppendParser(FFmpegMedia::ByteStreamFormat format, size_t partialStep)
    : m_format(format)
    , m_scanner(format, partialStep)
{
}

FFmpegAppendParser::~FFmpegAppendParser() = default;

const AVInputFormat* FFmpegAppendParser::inputFormat() const
{
    return av_find_input_format(m_format == FFmpegMedia::ByteStreamFormat::MP4 ? "mov" : "matroska");
}

void FFmpegAppendParser::reset()
{
    m_scanner.reset();
    m_error.clear();
    m_partialFragment = 0;
    m_partialTaken.clear();
}

bool FFmpegAppendParser::append(const uint8_t* data, size_t size, FFmpegAppendParserClient& client)
{
    m_lastAppendSamples = 0;
    m_lastAppendPartialSamples = 0;
    m_error.clear();
    std::vector<FFmpegMedia::ByteStreamSegment> segments;
    bool scanned = m_scanner.append(data, size, segments);
    for (auto& segment : segments) {
        bool ok = segment.kind == FFmpegMedia::ByteStreamSegment::Kind::Initialization
            ? parseInitialization(segment.bytes, client)
            : demuxMedia(segment, client);
        if (!ok)
            return false;
    }
    if (!scanned) {
        m_error = m_scanner.error();
        // The scanner stays failed until resetParserState().
        m_scanner.reset();
        return false;
    }
    return true;
}

bool FFmpegAppendParser::parseInitialization(const std::vector<uint8_t>& bytes, FFmpegAppendParserClient& client)
{
    const AVInputFormat* format = inputFormat();
    if (!format) {
        m_error = "libavformat lacks the mov/matroska demuxer";
        return false;
    }
    // The matroska demuxer only finishes its header at the first Cluster
    // (and fails at the end of the input): give it an empty one.
    static const std::vector<uint8_t> emptyCluster { 0x1F, 0x43, 0xB6, 0x75, 0x80 };
    bool isWebM = m_format == FFmpegMedia::ByteStreamFormat::WebM;
    MemoryDemuxer demuxer(bytes, isWebM ? &emptyCluster : nullptr, !isWebM);
    int result = demuxer.open(format);
    if (result < 0) {
        char buffer[AV_ERROR_MAX_STRING_SIZE] = { };
        m_error = std::string("initialization segment: ") + errorText(result, buffer);
        return false;
    }
    auto* context = demuxer.context();

    FFmpegParsedInitializationSegment segment;
    segment.durationSeconds = context->duration > 0 ? context->duration / static_cast<double>(AV_TIME_BASE) : std::numeric_limits<double>::quiet_NaN();
    for (unsigned i = 0; i < context->nb_streams; ++i) {
        AVStream* stream = context->streams[i];
        auto type = stream->codecpar->codec_type;
        if (type != AVMEDIA_TYPE_VIDEO && type != AVMEDIA_TYPE_AUDIO)
            continue;
        FFmpegParsedTrack track;
        track.trackID = trackIDForStream(stream);
        track.type = type;
        track.timeBase = stream->time_base;
        track.sampleAspectRatio = stream->sample_aspect_ratio.num ? stream->sample_aspect_ratio : stream->codecpar->sample_aspect_ratio;
        track.parameters = avcodec_parameters_alloc();
        if (!track.parameters || avcodec_parameters_copy(track.parameters, stream->codecpar) < 0) {
            m_error = "out of memory";
            return false;
        }
        const AVCodecDescriptor* descriptor = avcodec_descriptor_get(stream->codecpar->codec_id);
        track.codecName = descriptor ? descriptor->name : "?";
        track.fourCC = std::string(FFmpegMedia::fourCCForCodecName(track.codecName));
        segment.tracks.push_back(track);
    }
    if (segment.tracks.empty()) {
        m_error = "initialization segment has no audio or video track";
        return false;
    }

    m_initialization = bytes;
    m_partialFragment = 0;
    m_partialTaken.clear();
    m_current.tracks.clear();
    for (auto& track : segment.tracks)
        m_current.tracks.push_back(track);
    m_current.durationSeconds = segment.durationSeconds;
    client.appendParserDidParseInitializationSegment(m_current);
    return true;
}

bool FFmpegAppendParser::demuxMedia(const FFmpegMedia::ByteStreamSegment& segment, FFmpegAppendParserClient& client)
{
    const bool partial = segment.partial;
    if (m_initialization.empty()) {
        m_error = "media segment before the initialization segment";
        return false;
    }
    // Samples of a partially emitted fragment that were already delivered.
    std::vector<size_t> skip(m_current.tracks.size(), 0);
    if (segment.fragment && segment.fragment == m_partialFragment) {
        for (size_t t = 0; t < skip.size() && t < m_partialTaken.size(); ++t)
            skip[t] = m_partialTaken[t];
    }
    if (!partial) {
        m_partialFragment = 0;
        m_partialTaken.clear();
    }

    const AVInputFormat* format = inputFormat();
    // Matroska would follow SeekHead/Cues offsets of the original file on a
    // seekable input; the concatenated segment is read front to back.
    MemoryDemuxer demuxer(m_initialization, &segment.bytes, m_format == FFmpegMedia::ByteStreamFormat::MP4);
    int result = demuxer.open(format);
    if (result < 0) {
        if (partial)
            return true; // retried with more data
        char buffer[AV_ERROR_MAX_STRING_SIZE] = { };
        m_error = std::string("media segment: ") + errorText(result, buffer);
        return false;
    }
    auto* context = demuxer.context();
    const int64_t inputSize = static_cast<int64_t>(m_initialization.size() + segment.bytes.size());

    // stream index -> track of the current initialization segment
    std::vector<int> trackForStream(context->nb_streams, -1);
    for (unsigned i = 0; i < context->nb_streams; ++i) {
        uint64_t id = trackIDForStream(context->streams[i]);
        for (size_t t = 0; t < m_current.tracks.size(); ++t) {
            if (m_current.tracks[t].trackID == id)
                trackForStream[i] = static_cast<int>(t);
        }
    }

    // Collect first: missing durations are filled per track from the next
    // sample's decode time.
    struct Collected {
        int track;
        AVPacket* packet;
        bool hadDuration;
    };
    std::vector<Collected> collected;
    // Partial: a track stops at its first sample that is not complete.
    std::vector<bool> trackStopped(m_current.tracks.size(), false);
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        m_error = "out of memory";
        return false;
    }
    int readError = 0;
    while (true) {
        result = av_read_frame(context, packet);
        if (result < 0) {
            if (result != AVERROR_EOF)
                readError = result;
            break;
        }
        int track = packet->stream_index >= 0 && static_cast<unsigned>(packet->stream_index) < trackForStream.size() ? trackForStream[packet->stream_index] : -1;
        if (track >= 0 && partial) {
            // A sample cut by the end of the data is read short (and may not
            // be flagged corrupt): one reaching the end is not trusted yet.
            bool complete = !(packet->flags & AV_PKT_FLAG_CORRUPT) && packet->pos >= 0 && packet->pos + packet->size < inputSize;
            if (!complete)
                trackStopped[track] = true;
            if (trackStopped[track]) {
                av_packet_unref(packet);
                continue;
            }
        }
        if (track < 0 || (packet->flags & AV_PKT_FLAG_CORRUPT) || (packet->dts == AV_NOPTS_VALUE && packet->pts == AV_NOPTS_VALUE)) {
            av_packet_unref(packet);
            continue;
        }
        if (packet->dts == AV_NOPTS_VALUE)
            packet->dts = packet->pts;
        if (packet->pts == AV_NOPTS_VALUE)
            packet->pts = packet->dts;
        AVPacket* owned = av_packet_alloc();
        if (!owned) {
            av_packet_unref(packet);
            break;
        }
        av_packet_move_ref(owned, packet);
        collected.push_back({ track, owned, owned->duration > 0 });
    }
    av_packet_free(&packet);

    std::vector<size_t> taken(m_current.tracks.size(), 0);
    std::vector<bool> emit(collected.size(), false);
    for (size_t t = 0; t < m_current.tracks.size(); ++t) {
        auto& track = m_current.tracks[t];
        std::vector<FFmpegMedia::SampleTiming> timings;
        for (auto& entry : collected) {
            if (entry.track == static_cast<int>(t))
                timings.push_back({ entry.packet->dts, entry.packet->pts, entry.packet->duration });
        }
        if (timings.empty())
            continue;
        // Fallback: one video frame at 30 fps, or one audio frame (20 ms
        // when the codec does not say).
        double fallbackSeconds = 1.0 / 30;
        if (track.type == AVMEDIA_TYPE_AUDIO) {
            fallbackSeconds = 0.02;
            if (track.parameters && track.parameters->frame_size > 0 && track.parameters->sample_rate > 0)
                fallbackSeconds = track.parameters->frame_size / static_cast<double>(track.parameters->sample_rate);
        }
        int64_t fallback = track.timeBase.num > 0 ? std::max<int64_t>(1, std::llround(fallbackSeconds * track.timeBase.den / track.timeBase.num)) : 1;
        FFmpegMedia::fillMissingDurations(timings, fallback);
        // Partial: the last sample's duration needs the next one's decode
        // time unless the container gave it.
        size_t usable = timings.size();
        if (partial && !collected.empty()) {
            for (auto it = collected.rbegin(); it != collected.rend(); ++it) {
                if (it->track == static_cast<int>(t)) {
                    if (!it->hadDuration)
                        --usable;
                    break;
                }
            }
        }
        size_t index = 0;
        for (size_t i = 0; i < collected.size(); ++i) {
            auto& entry = collected[i];
            if (entry.track != static_cast<int>(t))
                continue;
            entry.packet->duration = timings[index].duration;
            emit[i] = index >= skip[t] && index < usable;
            ++index;
        }
        taken[t] = std::max(skip[t], usable);
    }

    unsigned emitted = 0;
    for (size_t i = 0; i < collected.size(); ++i) {
        auto& entry = collected[i];
        if (emit[i]) {
            client.appendParserDidParseSample(m_current.tracks[entry.track], entry.packet);
            ++emitted;
        }
        av_packet_free(&entry.packet);
    }
    m_lastAppendSamples += emitted;
    if (partial) {
        m_lastAppendPartialSamples += emitted;
        m_partialFragment = segment.fragment;
        m_partialTaken = taken;
        return true;
    }

    if (readError && collected.empty()) {
        char buffer[AV_ERROR_MAX_STRING_SIZE] = { };
        m_error = std::string("media segment: ") + errorText(readError, buffer);
        return false;
    }
    if (readError) {
        char buffer[AV_ERROR_MAX_STRING_SIZE] = { };
        ffmpegMediaLog("mse: media segment read error after %zu samples: %s", collected.size(), errorText(readError, buffer));
    }
    return true;
}

} // namespace WebCore

#endif // ENABLE(VIDEO) && USE(FFMPEG) && ENABLE(MEDIA_SOURCE)
