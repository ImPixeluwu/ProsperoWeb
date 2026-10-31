// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audio forwarding from the WebProcess to the UI process. On the PS5 only
// the foreground application (the UI process) is mixed to the audio output;
// a port the WebProcess opens with sceAudioOut is drained but never heard.
// The media engine therefore streams decoded PCM over loopback TCP to an
// audio server in the UI process (src/ps5/audio), which mixes every stream
// into one SDL device.
//
// Wire format. Integers are little-endian; samples are interleaved float32
// in the machine's byte order (both ends run on the same machine).
//
//   client -> server, once:  hello (16 bytes)
//     "PS5A", u16 version (1), u16 channels (2), u32 rate (48000),
//     u16 format (1 = f32), u16 reserved (0)
//   then frames:             u8 type, u8 reserved[3], u32 payload bytes, payload
//     client -> server:
//       1 pcm     interleaved float32, a whole number of frames
//       2 flush   drop everything queued (seek)
//       3 pause   u8: 1 = paused, 0 = playing
//       4 volume  float32 gain, 0..1
//       5 close
//     server -> client:
//       0x80 accepted  u32 stream id, u32 output latency in frames (after
//                      the mixer: the SDL buffers)
//       0x81 played    u64 frames retired in total, u32 frames queued. Every
//                      ~20 ms. "Retired" counts frames mixed to the output
//                      plus frames dropped by a flush or an overflow, so
//                      that sent - retired is exactly what is still queued.
//       0x82 rejected  (unsupported hello), then the server closes
//
// RemoteAudioClock turns the played reports into the sink position the
// engine's audio clock needs. Engine-independent (no WebCore/WTF), unit
// tested on the host by tests/ps5/test_audio_server.cpp.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace WebCore {
namespace FFmpegMedia {
namespace AudioWire {

constexpr int defaultPort = 17772;
constexpr uint16_t version = 1;
constexpr uint16_t formatF32 = 1;
constexpr uint32_t sampleRate = 48000;
constexpr uint16_t channels = 2;
constexpr size_t helloSize = 16;
constexpr size_t headerSize = 8;
// Larger payloads are a protocol error (a pcm chunk is ~10 ms).
constexpr uint32_t maxPayload = 1u << 20;

enum class Type : uint8_t {
    Pcm = 1,
    Flush = 2,
    Pause = 3,
    Volume = 4,
    Close = 5,
    Accepted = 0x80,
    Played = 0x81,
    Rejected = 0x82,
};

inline void putU16(uint8_t* out, uint16_t value)
{
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}

inline void putU32(uint8_t* out, uint32_t value)
{
    for (int i = 0; i < 4; ++i)
        out[i] = static_cast<uint8_t>(value >> (8 * i));
}

inline void putU64(uint8_t* out, uint64_t value)
{
    for (int i = 0; i < 8; ++i)
        out[i] = static_cast<uint8_t>(value >> (8 * i));
}

inline uint16_t getU16(const uint8_t* in)
{
    return static_cast<uint16_t>(in[0] | (in[1] << 8));
}

inline uint32_t getU32(const uint8_t* in)
{
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i)
        value |= static_cast<uint32_t>(in[i]) << (8 * i);
    return value;
}

inline uint64_t getU64(const uint8_t* in)
{
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(in[i]) << (8 * i);
    return value;
}

struct Hello {
    uint16_t version { AudioWire::version };
    uint16_t channels { AudioWire::channels };
    uint32_t rate { sampleRate };
    uint16_t format { formatF32 };
};

inline void encodeHello(const Hello& hello, uint8_t* out)
{
    memcpy(out, "PS5A", 4);
    putU16(out + 4, hello.version);
    putU16(out + 6, hello.channels);
    putU32(out + 8, hello.rate);
    putU16(out + 12, hello.format);
    putU16(out + 14, 0);
}

// nullopt when the magic is wrong; the caller validates the fields.
inline std::optional<Hello> decodeHello(const uint8_t* in)
{
    if (memcmp(in, "PS5A", 4))
        return std::nullopt;
    Hello hello;
    hello.version = getU16(in + 4);
    hello.channels = getU16(in + 6);
    hello.rate = getU32(in + 8);
    hello.format = getU16(in + 12);
    return hello;
}

inline bool helloSupported(const Hello& hello)
{
    return hello.version == version && hello.channels == channels && hello.rate == sampleRate && hello.format == formatF32;
}

inline void encodeHeader(Type type, uint32_t payloadBytes, uint8_t* out)
{
    out[0] = static_cast<uint8_t>(type);
    out[1] = out[2] = out[3] = 0;
    putU32(out + 4, payloadBytes);
}

// A whole message (header and payload) appended to `out`.
inline void appendMessage(std::vector<uint8_t>& out, Type type, const void* payload = nullptr, size_t bytes = 0)
{
    size_t start = out.size();
    out.resize(start + headerSize + bytes);
    encodeHeader(type, static_cast<uint32_t>(bytes), out.data() + start);
    if (bytes)
        memcpy(out.data() + start + headerSize, payload, bytes);
}

inline void appendPause(std::vector<uint8_t>& out, bool paused)
{
    uint8_t value = paused ? 1 : 0;
    appendMessage(out, Type::Pause, &value, 1);
}

inline void appendVolume(std::vector<uint8_t>& out, float gain)
{
    appendMessage(out, Type::Volume, &gain, sizeof(gain));
}

struct Accepted {
    uint32_t streamId { 0 };
    uint32_t outputLatencyFrames { 0 };
};

inline void appendAccepted(std::vector<uint8_t>& out, const Accepted& accepted)
{
    uint8_t payload[8];
    putU32(payload, accepted.streamId);
    putU32(payload + 4, accepted.outputLatencyFrames);
    appendMessage(out, Type::Accepted, payload, sizeof(payload));
}

inline std::optional<Accepted> decodeAccepted(const uint8_t* payload, size_t bytes)
{
    if (bytes != 8)
        return std::nullopt;
    return Accepted { getU32(payload), getU32(payload + 4) };
}

struct Played {
    uint64_t framesRetired { 0 };
    uint32_t queuedFrames { 0 };
};

inline void appendPlayed(std::vector<uint8_t>& out, const Played& played)
{
    uint8_t payload[12];
    putU64(payload, played.framesRetired);
    putU32(payload + 8, played.queuedFrames);
    appendMessage(out, Type::Played, payload, sizeof(payload));
}

inline std::optional<Played> decodePlayed(const uint8_t* payload, size_t bytes)
{
    if (bytes != 12)
        return std::nullopt;
    return Played { getU64(payload), getU32(payload + 8) };
}

inline std::optional<float> decodeVolume(const uint8_t* payload, size_t bytes)
{
    if (bytes != sizeof(float))
        return std::nullopt;
    float gain;
    memcpy(&gain, payload, sizeof(gain));
    if (!(gain >= 0))
        gain = 0;
    return std::min(gain, 1.0f);
}

// Incremental reader of a byte stream: optionally a hello first, then
// messages. A message's payload pointer stays valid until the next append().
class FrameParser {
public:
    explicit FrameParser(bool expectHello = false)
        : m_needHello(expectHello)
    {
    }

    void append(const uint8_t* data, size_t bytes)
    {
        if (m_offset && m_offset == m_buffer.size()) {
            m_buffer.clear();
            m_offset = 0;
        } else if (m_offset > 65536) {
            m_buffer.erase(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(m_offset));
            m_offset = 0;
        }
        m_buffer.insert(m_buffer.end(), data, data + bytes);
    }

    enum class Result { NeedMore, Hello, Message, Error };

    struct Message {
        Type type { Type::Close };
        const uint8_t* payload { nullptr };
        uint32_t bytes { 0 };
    };

    Result next()
    {
        if (m_error)
            return Result::Error;
        size_t available = m_buffer.size() - m_offset;
        const uint8_t* data = m_buffer.data() + m_offset;
        if (m_needHello) {
            if (available < helloSize)
                return Result::NeedMore;
            auto hello = decodeHello(data);
            if (!hello) {
                m_error = true;
                return Result::Error;
            }
            m_hello = *hello;
            m_needHello = false;
            m_offset += helloSize;
            return Result::Hello;
        }
        if (available < headerSize)
            return Result::NeedMore;
        uint32_t bytes = getU32(data + 4);
        if (bytes > maxPayload) {
            m_error = true;
            return Result::Error;
        }
        if (available < headerSize + bytes)
            return Result::NeedMore;
        m_message = { static_cast<Type>(data[0]), data + headerSize, bytes };
        m_offset += headerSize + bytes;
        return Result::Message;
    }

    const Hello& hello() const { return m_hello; }
    const Message& message() const { return m_message; }
    size_t pendingBytes() const { return m_buffer.size() - m_offset; }

private:
    std::vector<uint8_t> m_buffer;
    size_t m_offset { 0 };
    bool m_needHello;
    bool m_error { false };
    Hello m_hello;
    Message m_message;
};

// Client-side position of a remote stream. `sent` counts frames handed to
// the server; the server's played reports say how many of them it retired.
// Between reports (every ~20 ms) the position extrapolates in real time
// while playing, at most `maxExtrapolation` seconds past the last report, so
// a server that stops reporting stalls the clock instead of running away.
// estimatedRetired() never goes backwards and never passes `sent`.
class RemoteAudioClock {
public:
    static constexpr double maxExtrapolation = 0.1;

    explicit RemoteAudioClock(int rate = static_cast<int>(sampleRate))
        : m_rate(rate)
    {
    }

    void setOutputLatencyFrames(uint32_t frames) { m_outputLatencyFrames = frames; }

    void sent(uint64_t frames) { m_sent += frames; }
    uint64_t totalSent() const { return m_sent; }

    void report(const Played& played, double now)
    {
        m_reported = std::max(m_reported, std::min(played.framesRetired, m_sent));
        m_reportTime = now;
        m_haveReport = true;
    }

    // Everything sent so far will be dropped by the server (flush), or
    // there is no server any more (local pacing): count it as retired now.
    void retireAllSent() { m_floor = m_sent; }

    void setPlaying(bool playing, double now)
    {
        if (playing == m_playing)
            return;
        // Freeze the extrapolated part when pausing; restart its window
        // when resuming.
        m_last = estimatedRetired(now);
        m_reported = std::max(m_reported, m_last);
        m_playing = playing;
        m_resumeTime = now;
    }

    bool playing() const { return m_playing; }

    uint64_t estimatedRetired(double now)
    {
        uint64_t base = std::max(m_reported, m_floor);
        uint64_t estimate = base;
        if (m_playing && m_haveReport) {
            double since = std::max(m_reportTime, m_resumeTime);
            double elapsed = std::clamp(now - since, 0.0, maxExtrapolation);
            estimate = base + static_cast<uint64_t>(std::llround(elapsed * m_rate));
        }
        estimate = std::min(std::max(estimate, m_last), m_sent);
        m_last = estimate;
        return estimate;
    }

    // Frames sent but not heard yet, excluding the output latency.
    uint64_t aheadFrames(double now) { return m_sent - estimatedRetired(now); }

    // Seconds between handing a frame to the server and hearing it.
    double latencySeconds(double now)
    {
        if (m_rate <= 0)
            return 0;
        return static_cast<double>(aheadFrames(now) + m_outputLatencyFrames) / m_rate;
    }

private:
    int m_rate;
    uint32_t m_outputLatencyFrames { 0 };
    uint64_t m_sent { 0 };
    uint64_t m_reported { 0 };
    uint64_t m_floor { 0 };
    uint64_t m_last { 0 };
    double m_reportTime { 0 };
    double m_resumeTime { 0 };
    bool m_haveReport { false };
    bool m_playing { false };
};

} // namespace AudioWire
} // namespace FFmpegMedia
} // namespace WebCore
