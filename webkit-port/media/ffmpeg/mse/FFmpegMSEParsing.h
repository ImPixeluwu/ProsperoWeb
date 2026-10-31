// SPDX-License-Identifier: GPL-3.0-or-later
//
// Engine-independent byte stream handling of the ffmpeg MSE source buffers
// (no WebCore, WTF or ffmpeg headers; unit tested on the host by
// tests/media/):
//  - ByteQueue: growable byte buffer with the read/seek semantics an
//    AVIOContext needs;
//  - SegmentScanner: splits appended ISO BMFF (fragmented MP4) or WebM bytes
//    into complete initialization and media segments at top-level box /
//    element boundaries, keeping partial data for the next append;
//  - timestamp helpers: exact stream-timebase to (value, timescale)
//    conversion, microsecond conversion, and filling missing durations.
//
// FFmpegAppendParser feeds each complete segment to libavformat (mov or
// matroska demuxer over an in-memory AVIOContext), so libavformat never sees
// a truncated box and never blocks waiting for data.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace WebCore {
namespace FFmpegMedia {

// MARK: - ByteQueue

class ByteQueue {
public:
    void append(const uint8_t* data, size_t size)
    {
        if (!size)
            return;
        compactIfNeeded();
        m_bytes.insert(m_bytes.end(), data, data + size);
    }

    size_t size() const { return m_bytes.size() - m_front; }
    bool empty() const { return !size(); }
    const uint8_t* data() const { return m_bytes.data() + m_front; }

    // Drops `count` bytes from the front; the read position moves with the
    // data (it stays on the same byte, or at 0 if that byte was dropped).
    void consume(size_t count)
    {
        count = std::min(count, size());
        m_front += count;
        m_position = m_position > count ? m_position - count : 0;
        if (m_front == m_bytes.size())
            clear();
    }

    void clear()
    {
        m_bytes.clear();
        m_front = 0;
        m_position = 0;
    }

    // Reader (AVIOContext read_packet/seek). read() returns the number of
    // bytes copied, 0 at the end of the queue.
    size_t position() const { return m_position; }
    size_t read(uint8_t* out, size_t count)
    {
        size_t available = size() - std::min(m_position, size());
        count = std::min(count, available);
        if (count)
            memcpy(out, data() + m_position, count);
        m_position += count;
        return count;
    }

    enum class Whence { Set, Current, End };
    // Returns the new position, or -1 (position unchanged) when it would
    // fall outside [0, size()].
    int64_t seek(int64_t offset, Whence whence)
    {
        int64_t base = 0;
        if (whence == Whence::Current)
            base = static_cast<int64_t>(m_position);
        else if (whence == Whence::End)
            base = static_cast<int64_t>(size());
        int64_t target = base + offset;
        if (target < 0 || target > static_cast<int64_t>(size()))
            return -1;
        m_position = static_cast<size_t>(target);
        return target;
    }

private:
    void compactIfNeeded()
    {
        // Reclaim consumed bytes once they dominate the buffer.
        if (m_front && m_front >= (1u << 16) && m_front * 2 >= m_bytes.size()) {
            m_bytes.erase(m_bytes.begin(), m_bytes.begin() + static_cast<std::ptrdiff_t>(m_front));
            m_front = 0;
        }
    }

    std::vector<uint8_t> m_bytes;
    size_t m_front { 0 };
    size_t m_position { 0 };
};

// MARK: - Segments

enum class ByteStreamFormat { MP4, WebM };

struct ByteStreamSegment {
    enum class Kind { Initialization, Media };
    Kind kind { Kind::Media };
    std::vector<uint8_t> bytes;
};

inline uint32_t readBigEndian32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

inline uint64_t readBigEndian64(const uint8_t* p)
{
    return (static_cast<uint64_t>(readBigEndian32(p)) << 32) | readBigEndian32(p + 4);
}

inline constexpr uint32_t fourCC(const char (&code)[5])
{
    return (static_cast<uint32_t>(static_cast<uint8_t>(code[0])) << 24) | (static_cast<uint32_t>(static_cast<uint8_t>(code[1])) << 16)
        | (static_cast<uint32_t>(static_cast<uint8_t>(code[2])) << 8) | static_cast<uint8_t>(code[3]);
}

// EBML element IDs (with their length marker bits, as written in the file).
namespace WebMID {
inline constexpr uint32_t EBML = 0x1A45DFA3;
inline constexpr uint32_t Segment = 0x18538067;
inline constexpr uint32_t SeekHead = 0x114D9B74;
inline constexpr uint32_t Info = 0x1549A966;
inline constexpr uint32_t Tracks = 0x1654AE6B;
inline constexpr uint32_t Cues = 0x1C53BB6B;
inline constexpr uint32_t Cluster = 0x1F43B675;
inline constexpr uint32_t Tags = 0x1254C367;
inline constexpr uint32_t Chapters = 0x1043A770;
inline constexpr uint32_t Attachments = 0x1941A469;
inline constexpr uint32_t Void = 0xEC;
inline constexpr uint32_t CRC32 = 0xBF;
// Cluster children.
inline constexpr uint32_t Timecode = 0xE7;
inline constexpr uint32_t SilentTracks = 0x5854;
inline constexpr uint32_t Position = 0xA7;
inline constexpr uint32_t PrevSize = 0xAB;
inline constexpr uint32_t SimpleBlock = 0xA3;
inline constexpr uint32_t BlockGroup = 0xA0;
inline constexpr uint32_t EncryptedBlock = 0xAF;
} // namespace WebMID

struct EBMLHeader {
    uint32_t id { 0 };
    uint64_t size { 0 };
    size_t headerLength { 0 };
    bool unknownSize { false };
};

enum class ParseStatus { Ok, NeedMoreData, Invalid };

// Reads an EBML element header (ID and data size VINTs) at `p`.
inline ParseStatus readEBMLHeader(const uint8_t* p, size_t available, EBMLHeader& header)
{
    if (!available)
        return ParseStatus::NeedMoreData;
    unsigned idLength = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (p[0] & (0x80 >> i)) {
            idLength = i + 1;
            break;
        }
    }
    if (!idLength)
        return ParseStatus::Invalid;
    if (available < idLength + 1)
        return ParseStatus::NeedMoreData;
    uint32_t id = 0;
    for (unsigned i = 0; i < idLength; ++i)
        id = (id << 8) | p[i];
    uint8_t first = p[idLength];
    unsigned sizeLength = 0;
    for (unsigned i = 0; i < 8; ++i) {
        if (first & (0x80 >> i)) {
            sizeLength = i + 1;
            break;
        }
    }
    if (!sizeLength)
        return ParseStatus::Invalid;
    if (available < idLength + sizeLength)
        return ParseStatus::NeedMoreData;
    uint64_t size = first & (0xFF >> sizeLength);
    bool allOnes = size == (0xFFu >> sizeLength);
    for (unsigned i = 1; i < sizeLength; ++i) {
        size = (size << 8) | p[idLength + i];
        allOnes = allOnes && p[idLength + i] == 0xFF;
    }
    header.id = id;
    header.size = size;
    header.headerLength = idLength + sizeLength;
    header.unknownSize = allOnes;
    return ParseStatus::Ok;
}

// Splits an appended byte stream into complete segments.
//  MP4:  ftyp ... moov  -> initialization segment (only ftyp and moov kept);
//        moof + mdat    -> media data, emitted per append (styp, sidx, ssix,
//                          prft, emsg, free, skip, mfra are dropped).
//  WebM: EBML header + Segment header + Info + Tracks -> initialization
//        segment (SeekHead, Void, Tags, ... dropped: their offsets would not
//        match); Cluster (known or unknown size) -> media data; Cues and other
//        level-1 elements after the Tracks are dropped.
// A media segment before any initialization segment is an error, as is a
// malformed box or element.
class SegmentScanner {
public:
    explicit SegmentScanner(ByteStreamFormat format)
        : m_format(format)
    {
    }

    ByteStreamFormat format() const { return m_format; }
    bool hasInitializationSegment() const { return m_hasInitialization; }
    size_t pendingBytes() const { return m_pending.size() + m_partialMedia.size(); }
    const std::string& error() const { return m_error; }

    // Returns false on a parse error (error() says why); segments completed
    // before the error are still appended to `out`.
    bool append(const uint8_t* data, size_t size, std::vector<ByteStreamSegment>& out)
    {
        if (!m_error.empty())
            return false;
        m_pending.append(data, size);
        bool ok = m_format == ByteStreamFormat::MP4 ? scanMP4(out) : scanWebM(out);
        if (!m_media.empty()) {
            ByteStreamSegment segment;
            segment.kind = ByteStreamSegment::Kind::Media;
            segment.bytes.swap(m_media);
            out.push_back(segment);
        }
        return ok;
    }

    // resetParserState(): drops partial data. The last initialization segment
    // stays in effect for the media segments that follow.
    void reset()
    {
        m_pending.clear();
        m_partialMedia.clear();
        m_media.clear();
        m_init.clear();
        m_inInitialization = false;
        m_inSegment = false;
        m_skipRemaining = 0;
        m_error.clear();
    }

private:
    static constexpr uint64_t maxElementSize = 1ull << 30;

    bool fail(const char* message)
    {
        m_error = message;
        m_pending.clear();
        return false;
    }

    void emitInitialization(std::vector<ByteStreamSegment>& out)
    {
        // Media of the previous initialization segment goes first.
        if (!m_media.empty()) {
            ByteStreamSegment media;
            media.kind = ByteStreamSegment::Kind::Media;
            media.bytes.swap(m_media);
            out.push_back(media);
        }
        ByteStreamSegment segment;
        segment.kind = ByteStreamSegment::Kind::Initialization;
        segment.bytes.swap(m_init);
        out.push_back(segment);
        m_inInitialization = false;
        m_hasInitialization = true;
    }

    void take(std::vector<uint8_t>& destination, size_t count)
    {
        destination.insert(destination.end(), m_pending.data(), m_pending.data() + count);
        m_pending.consume(count);
    }

    bool scanMP4(std::vector<ByteStreamSegment>& out)
    {
        while (m_pending.size() >= 8) {
            const uint8_t* p = m_pending.data();
            uint64_t size = readBigEndian32(p);
            uint32_t type = readBigEndian32(p + 4);
            size_t headerLength = 8;
            if (size == 1) {
                if (m_pending.size() < 16)
                    return true;
                size = readBigEndian64(p + 8);
                headerLength = 16;
            } else if (!size)
                return fail("MP4 box extends to the end of the stream");
            if (size < headerLength)
                return fail("MP4 box smaller than its header");
            if (size > maxElementSize)
                return fail("MP4 box larger than 1 GiB");
            if (m_pending.size() < size)
                return true;

            if (type == fourCC("ftyp")) {
                m_init.clear();
                m_partialMedia.clear();
                m_inInitialization = true;
                take(m_init, size);
            } else if (type == fourCC("moov")) {
                if (!m_inInitialization)
                    m_init.clear();
                take(m_init, size);
                emitInitialization(out);
            } else if (type == fourCC("moof")) {
                if (!m_hasInitialization || m_inInitialization)
                    return fail("media segment before the initialization segment");
                m_partialMedia.clear();
                take(m_partialMedia, size);
            } else if (type == fourCC("mdat")) {
                if (!m_hasInitialization || m_inInitialization)
                    return fail("media segment before the initialization segment");
                if (m_partialMedia.empty()) {
                    // mdat without a moof: nothing references it.
                    m_pending.consume(size);
                    continue;
                }
                m_media.insert(m_media.end(), m_partialMedia.begin(), m_partialMedia.end());
                m_partialMedia.clear();
                take(m_media, size);
            } else
                m_pending.consume(size);
        }
        return true;
    }

    bool isLevelOneElement(uint32_t id) const
    {
        return id == WebMID::SeekHead || id == WebMID::Info || id == WebMID::Tracks || id == WebMID::Cues || id == WebMID::Cluster
            || id == WebMID::Tags || id == WebMID::Chapters || id == WebMID::Attachments || id == WebMID::EBML || id == WebMID::Segment;
    }

    static bool isClusterChild(uint32_t id)
    {
        return id == WebMID::Timecode || id == WebMID::SilentTracks || id == WebMID::Position || id == WebMID::PrevSize
            || id == WebMID::SimpleBlock || id == WebMID::BlockGroup || id == WebMID::EncryptedBlock || id == WebMID::Void || id == WebMID::CRC32;
    }

    // Length of an unknown-size cluster starting at the front of m_pending,
    // or 0 while its end is not known yet.
    size_t unknownSizeClusterLength(size_t headerLength, ParseStatus& status) const
    {
        const uint8_t* p = m_pending.data();
        size_t available = m_pending.size();
        size_t offset = headerLength;
        status = ParseStatus::Ok;
        while (offset < available) {
            EBMLHeader child;
            auto result = readEBMLHeader(p + offset, available - offset, child);
            if (result == ParseStatus::NeedMoreData)
                return 0;
            if (result == ParseStatus::Invalid) {
                status = ParseStatus::Invalid;
                return 0;
            }
            if (!isClusterChild(child.id))
                return offset;
            if (child.unknownSize || child.size > maxElementSize) {
                status = ParseStatus::Invalid;
                return 0;
            }
            offset += child.headerLength + child.size;
        }
        return 0;
    }

    bool scanWebM(std::vector<ByteStreamSegment>& out)
    {
        while (!m_pending.empty()) {
            if (m_skipRemaining) {
                size_t count = static_cast<size_t>(std::min<uint64_t>(m_skipRemaining, m_pending.size()));
                m_pending.consume(count);
                m_skipRemaining -= count;
                continue;
            }
            EBMLHeader header;
            auto status = readEBMLHeader(m_pending.data(), m_pending.size(), header);
            if (status == ParseStatus::NeedMoreData)
                return true;
            if (status == ParseStatus::Invalid)
                return fail("malformed WebM element header");

            if (header.id == WebMID::Segment) {
                // Entered, not skipped: its children are the level-1 elements.
                if (!m_inInitialization)
                    return fail("WebM Segment without an EBML header");
                take(m_init, header.headerLength);
                m_inSegment = true;
                continue;
            }

            uint64_t total = header.headerLength + header.size;
            if (header.id == WebMID::Cluster) {
                if (!m_hasInitialization || m_inInitialization)
                    return fail("media segment before the initialization segment");
                if (header.unknownSize) {
                    ParseStatus clusterStatus;
                    size_t length = unknownSizeClusterLength(header.headerLength, clusterStatus);
                    if (clusterStatus == ParseStatus::Invalid)
                        return fail("malformed WebM cluster");
                    if (!length)
                        return true;
                    take(m_media, length);
                    continue;
                }
                if (header.size > maxElementSize)
                    return fail("WebM cluster larger than 1 GiB");
                if (m_pending.size() < total)
                    return true;
                take(m_media, static_cast<size_t>(total));
                continue;
            }

            if (header.unknownSize)
                return fail("unknown-size WebM element outside a cluster");
            if (header.size > maxElementSize)
                return fail("WebM element larger than 1 GiB");

            if (header.id == WebMID::EBML) {
                if (m_pending.size() < total)
                    return true;
                m_init.clear();
                m_inInitialization = true;
                m_inSegment = false;
                take(m_init, static_cast<size_t>(total));
                continue;
            }

            if (m_inInitialization && (header.id == WebMID::Info || header.id == WebMID::Tracks)) {
                if (m_pending.size() < total)
                    return true;
                take(m_init, static_cast<size_t>(total));
                if (header.id == WebMID::Tracks)
                    emitInitialization(out);
                continue;
            }

            if (!m_inSegment && !isLevelOneElement(header.id) && header.id != WebMID::Void && header.id != WebMID::CRC32)
                return fail("unexpected WebM element");
            // Everything else (SeekHead, Void, Cues, Tags, ...) is skipped,
            // possibly across appends.
            size_t now = static_cast<size_t>(std::min<uint64_t>(total, m_pending.size()));
            m_pending.consume(now);
            m_skipRemaining = total - now;
        }
        return true;
    }

    const ByteStreamFormat m_format;
    ByteQueue m_pending;
    std::vector<uint8_t> m_init;
    std::vector<uint8_t> m_partialMedia; // MP4: a moof waiting for its mdat
    std::vector<uint8_t> m_media; // complete media data of the current append
    uint64_t m_skipRemaining { 0 };
    bool m_inInitialization { false };
    bool m_inSegment { false };
    bool m_hasInitialization { false };
    std::string m_error;
};

// MARK: - Timestamps

struct TimeValue {
    int64_t value { 0 };
    uint32_t timescale { 1 };
    bool valid { false };
};

// A timestamp in units of `timeBaseNum / timeBaseDen` seconds as an exact
// (value, timescale) pair, i.e. a MediaTime. Invalid on overflow or a bad
// timebase.
inline TimeValue timeValueFromTimestamp(int64_t timestamp, int timeBaseNum, int timeBaseDen)
{
    TimeValue result;
    if (timeBaseNum <= 0 || timeBaseDen <= 0)
        return result;
    if (timestamp && (timestamp > std::numeric_limits<int64_t>::max() / timeBaseNum || timestamp < std::numeric_limits<int64_t>::min() / timeBaseNum))
        return result;
    result.value = timestamp * timeBaseNum;
    result.timescale = static_cast<uint32_t>(timeBaseDen);
    result.valid = true;
    return result;
}

// value / timescale seconds in microseconds, rounded to nearest (the decoder
// queues of the MSE player use a 1/1000000 timebase).
inline int64_t microsecondsFromTimeValue(int64_t value, uint32_t timescale)
{
    if (!timescale)
        return 0;
    __int128 scaled = static_cast<__int128>(value) * 1000000;
    __int128 half = timescale / 2;
    __int128 rounded = scaled >= 0 ? (scaled + half) / timescale : -((-scaled + half) / timescale);
    if (rounded > std::numeric_limits<int64_t>::max())
        return std::numeric_limits<int64_t>::max();
    if (rounded < std::numeric_limits<int64_t>::min())
        return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(rounded);
}

inline int64_t microsecondsFromSeconds(double seconds)
{
    if (!(seconds == seconds))
        return 0;
    double scaled = seconds * 1e6;
    if (scaled >= 9.2e18)
        return std::numeric_limits<int64_t>::max();
    if (scaled <= -9.2e18)
        return std::numeric_limits<int64_t>::min();
    return static_cast<int64_t>(scaled >= 0 ? scaled + 0.5 : scaled - 0.5);
}

struct SampleTiming {
    int64_t dts { 0 };
    int64_t pts { 0 };
    int64_t duration { 0 };
};

// Matroska blocks without BlockDuration (and the odd MP4 trun) carry no
// duration, but MSE needs presentation intervals. Samples of one track in
// decode order: a missing duration becomes the gap to the next sample's
// decode time, then the previous known duration, then `fallback`.
inline void fillMissingDurations(std::vector<SampleTiming>& samples, int64_t fallback)
{
    int64_t previous = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        auto& sample = samples[i];
        if (sample.duration <= 0 && i + 1 < samples.size()) {
            int64_t gap = samples[i + 1].dts - sample.dts;
            if (gap > 0)
                sample.duration = gap;
        }
        if (sample.duration <= 0)
            sample.duration = previous > 0 ? previous : fallback;
        previous = sample.duration;
    }
}

// The 4-character code a SourceBuffer reports for a track (MediaDescription
// codec; FourCC::fromString on the WebCore side). Profiles and levels are
// deliberately left out, so switching between H.264 Main and High (YouTube
// quality changes) is not a codec change. `codecName` is libavcodec's
// AVCodecDescriptor name.
inline std::string_view fourCCForCodecName(std::string_view codecName)
{
    if (codecName == "h264")
        return "avc1";
    if (codecName == "hevc")
        return "hvc1";
    if (codecName == "vp9")
        return "vp09";
    if (codecName == "vp8")
        return "vp08";
    if (codecName == "av1")
        return "av01";
    if (codecName == "aac" || codecName == "aac_latm")
        return "mp4a";
    if (codecName == "mp3")
        return ".mp3";
    if (codecName == "opus")
        return "opus";
    if (codecName == "vorbis")
        return "vorb";
    if (codecName == "flac")
        return "fLaC";
    if (codecName == "ac3")
        return "ac-3";
    if (codecName == "eac3")
        return "ec-3";
    return "????";
}

} // namespace FFmpegMedia
} // namespace WebCore
