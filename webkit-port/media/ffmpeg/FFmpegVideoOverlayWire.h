// SPDX-License-Identifier: GPL-3.0-or-later
//
// Video overlay forwarding from the WebProcess to the UI process. Without
// accelerated compositing every video frame used to be painted into the
// page's backing store (Skia scaling the frame into the element), copied to
// the UI over WebKit IPC and copied again into the UI's caches. In overlay
// mode the video element paints a transparent hole instead, and the frames
// travel to the UI process here; the UI composes them under the page.
//
// Control channel: loopback TCP (PROSPEROWEB_VIDEO_PORT, default 17773), one
// connection per player. Pixels go through shared memory when both processes
// can map the same object (a named POSIX shm object, else a file under the
// data directory mapped MAP_SHARED), otherwise inline over the socket.
//
// Wire format. Integers little-endian, f64 in the machine's byte order (both
// ends run on the same machine).
//
//   client -> server, once:  hello (16 bytes)
//     "PS5V", u16 version (1), u16 reserved, u32 pid, u32 player id
//   then messages:           u8 type, u8 reserved[3], u32 payload bytes, payload
//     client -> server:
//       1 shm       u8 kind (1 = shm_open name, 2 = file path), u8[3], u32 slots,
//                   u32 slot bytes, name bytes (the rest). The server maps it
//                   and answers shm-result; the client unlinks the name then.
//       2 geometry  u64 page id, i32 x, y, w, h (view coordinates of the video
//                   box, unclipped), u8 visible, u8 fullscreen, u8[2]
//       3 present   u32 slot, u32 seq, u32 width, u32 height, u32 stride, f64 pts:
//                   the frame in `slot` (written under sequence number `seq`)
//                   is the one to show now
//       4 frame     u32 width, u32 height, u32 stride, u32 reserved, f64 pts,
//                   then height * stride bytes of pixels (no shared memory)
//       5 clear     drop the current picture
//       6 close
//     server -> client:
//       0x80 accepted    u32 player key (unique in the server)
//       0x81 shm-result  u8 ok, u8[3], u32 errno of the failed open/mmap
//
// Remote decoding (PROSPEROWEB_VIDEO_DECODE=ui): instead of pixels the
// player sends the compressed stream, the UI decodes it (sceVideodec2) and
// keeps the decoded pictures; the player's presenter still decides which
// frame is due and names it by its pts. Every message carries the stream id
// of the stream-open it belongs to, so late messages of a closed stream are
// ignored. Timestamps are integers in the stream's timebase, echoed
// unchanged, so the frame the player presents is matched exactly.
//     client -> server:
//       7  stream-open  u32 stream, u8 codec, u8 matrix (0 unknown, 1 BT.601,
//                       2 BT.709, 3 BT.2020), u8 full range, u8, i32 profile,
//                       i32 level, u32 width, u32 height, i32 timebase num,
//                       den, then the extradata (avcC/hvcC as the container
//                       has it, Annex-B, or nothing). Sent again with the same
//                       stream id when the extradata changes (new avcC on a
//                       quality switch): the UI reconfigures, no answer.
//       8  packet       u32 stream, u32 serial, i64 pts, i64 dts, u8 flags
//                       (1 = keyframe), u8[3], then the packet's bytes as the
//                       demuxer produced them (length-prefixed or Annex-B;
//                       the UI converts, see stream-open)
//       9  flush        u32 stream, u32 serial: a seek; drop everything queued
//                       and decoded, reset the decoder, continue with `serial`
//       10 show         u32 stream, u32 serial, i64 pts: present the newest
//                       decoded frame at or before pts; older ones are released
//       11 discard      u32 stream, u32 serial, i64 pts: the player dropped
//                       that frame (late, before a seek target): release it
//       12 drain        u32 stream, u32 serial: end of stream, output what the
//                       decoder still holds, then answer decoded(drained)
//       13 stream-close u32 stream
//     server -> client:
//       0x82 stream-ok     u32 stream, u8 mode (1 = hardware), u8[3],
//                          u32 frames the UI holds at most
//       0x83 stream-reject u32 stream, reason text: the player decodes itself
//       0x84 decoded       u32 stream, u32 serial, u32 packets consumed (total
//                          for the stream, decoded or dropped), u8 flags
//                          (1 = frame, 2 = drained), u8[3], u32 width,
//                          u32 height, i64 pts
//       0x85 stream-error  u32 stream, reason text: decoding failed at run
//                          time; the player falls back to its own decoder
//
// Pixels are BGRA bytes (premultiplied ARGB32 words on little-endian; video
// is opaque), the layout the decoder's swscale writes.
//
// Shared memory layout (ShmLayout): a 4 KiB header page ("PS5VSHM1", u32
// slots, u32 slot bytes, then one u32 sequence counter per slot at byte 64 +
// 4 * slot), followed by the slots. A slot's counter is odd while the writer
// fills it and even afterwards (a seqlock): the reader copies the slot and
// keeps the copy only if the counter equals the announced `seq` before and
// after the copy.
//
// Engine-independent (no WebCore/WTF), shared by the WebProcess client
// (FFmpegVideoOverlayClient.h), the UI server (src/ps5/video) and the host
// tests (tests/ps5/test_video_overlay.cpp).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace WebCore {
namespace FFmpegMedia {
namespace VideoWire {

constexpr int defaultPort = 17773;
constexpr uint16_t version = 1;
constexpr size_t helloSize = 16;
constexpr size_t headerSize = 8;
// A 4K BGRA frame plus its header; anything larger is a protocol error.
constexpr uint32_t maxPayload = 3840u * 2160u * 4u + 64u;
constexpr uint32_t maxFrameWidth = 3840;
constexpr uint32_t maxFrameHeight = 2160;

enum class Type : uint8_t {
    Shm = 1,
    Geometry = 2,
    Present = 3,
    Frame = 4,
    Clear = 5,
    Close = 6,
    StreamOpen = 7,
    Packet = 8,
    Flush = 9,
    Show = 10,
    Discard = 11,
    Drain = 12,
    StreamClose = 13,
    Accepted = 0x80,
    ShmResult = 0x81,
    StreamOk = 0x82,
    StreamReject = 0x83,
    Decoded = 0x84,
    StreamError = 0x85,
};

enum class ShmKind : uint8_t {
    Named = 1, // shm_open() name
    File = 2, // a file mapped MAP_SHARED
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

inline void putF64(uint8_t* out, double value)
{
    memcpy(out, &value, sizeof(value));
}

inline double getF64(const uint8_t* in)
{
    double value;
    memcpy(&value, in, sizeof(value));
    return value;
}

struct Hello {
    uint16_t version { VideoWire::version };
    uint32_t pid { 0 };
    uint32_t player { 0 };
};

inline void encodeHello(const Hello& hello, uint8_t* out)
{
    memcpy(out, "PS5V", 4);
    putU16(out + 4, hello.version);
    putU16(out + 6, 0);
    putU32(out + 8, hello.pid);
    putU32(out + 12, hello.player);
}

inline std::optional<Hello> decodeHello(const uint8_t* in)
{
    if (memcmp(in, "PS5V", 4))
        return std::nullopt;
    Hello hello;
    hello.version = getU16(in + 4);
    hello.pid = getU32(in + 8);
    hello.player = getU32(in + 12);
    return hello;
}

inline void encodeHeader(Type type, uint32_t payloadBytes, uint8_t* out)
{
    out[0] = static_cast<uint8_t>(type);
    out[1] = out[2] = out[3] = 0;
    putU32(out + 4, payloadBytes);
}

inline void appendMessage(std::vector<uint8_t>& out, Type type, const void* payload = nullptr, size_t bytes = 0)
{
    size_t offset = out.size();
    out.resize(offset + headerSize + bytes);
    encodeHeader(type, static_cast<uint32_t>(bytes), out.data() + offset);
    if (bytes)
        memcpy(out.data() + offset + headerSize, payload, bytes);
}

struct ShmOffer {
    ShmKind kind { ShmKind::Named };
    uint32_t slots { 0 };
    uint32_t slotBytes { 0 };
    std::string name;
};

inline void appendShmOffer(std::vector<uint8_t>& out, const ShmOffer& offer)
{
    std::vector<uint8_t> payload(12 + offer.name.size());
    payload[0] = static_cast<uint8_t>(offer.kind);
    putU32(payload.data() + 4, offer.slots);
    putU32(payload.data() + 8, offer.slotBytes);
    memcpy(payload.data() + 12, offer.name.data(), offer.name.size());
    appendMessage(out, Type::Shm, payload.data(), payload.size());
}

inline std::optional<ShmOffer> decodeShmOffer(const uint8_t* payload, size_t bytes)
{
    if (bytes < 13 || bytes > 12 + 1024)
        return std::nullopt;
    ShmOffer offer;
    if (payload[0] != static_cast<uint8_t>(ShmKind::Named) && payload[0] != static_cast<uint8_t>(ShmKind::File))
        return std::nullopt;
    offer.kind = static_cast<ShmKind>(payload[0]);
    offer.slots = getU32(payload + 4);
    offer.slotBytes = getU32(payload + 8);
    offer.name.assign(reinterpret_cast<const char*>(payload + 12), bytes - 12);
    if (!offer.slots || offer.slots > 8 || !offer.slotBytes || offer.slotBytes > maxPayload)
        return std::nullopt;
    if (offer.name.find('\0') != std::string::npos)
        return std::nullopt;
    return offer;
}

struct ShmResult {
    bool ok { false };
    uint32_t error { 0 };
};

inline void appendShmResult(std::vector<uint8_t>& out, const ShmResult& result)
{
    uint8_t payload[8] = { };
    payload[0] = result.ok ? 1 : 0;
    putU32(payload + 4, result.error);
    appendMessage(out, Type::ShmResult, payload, sizeof(payload));
}

inline std::optional<ShmResult> decodeShmResult(const uint8_t* payload, size_t bytes)
{
    if (bytes != 8)
        return std::nullopt;
    return ShmResult { payload[0] == 1, getU32(payload + 4) };
}

inline void appendAccepted(std::vector<uint8_t>& out, uint32_t key)
{
    uint8_t payload[4];
    putU32(payload, key);
    appendMessage(out, Type::Accepted, payload, sizeof(payload));
}

inline std::optional<uint32_t> decodeAccepted(const uint8_t* payload, size_t bytes)
{
    if (bytes != 4)
        return std::nullopt;
    return getU32(payload);
}

// The video box in view coordinates (the WKView's, i.e. the page's root
// view), not clipped: the UI clips to what it shows.
struct Geometry {
    uint64_t pageId { 0 };
    int32_t x { 0 };
    int32_t y { 0 };
    int32_t width { 0 };
    int32_t height { 0 };
    bool visible { false };
    bool fullscreen { false };

    bool operator==(const Geometry&) const = default;
};

constexpr size_t geometryBytes = 28;

inline void appendGeometry(std::vector<uint8_t>& out, const Geometry& geometry)
{
    uint8_t payload[geometryBytes] = { };
    putU64(payload, geometry.pageId);
    putU32(payload + 8, static_cast<uint32_t>(geometry.x));
    putU32(payload + 12, static_cast<uint32_t>(geometry.y));
    putU32(payload + 16, static_cast<uint32_t>(geometry.width));
    putU32(payload + 20, static_cast<uint32_t>(geometry.height));
    payload[24] = geometry.visible ? 1 : 0;
    payload[25] = geometry.fullscreen ? 1 : 0;
    appendMessage(out, Type::Geometry, payload, sizeof(payload));
}

inline std::optional<Geometry> decodeGeometry(const uint8_t* payload, size_t bytes)
{
    if (bytes != geometryBytes)
        return std::nullopt;
    Geometry geometry;
    geometry.pageId = getU64(payload);
    geometry.x = static_cast<int32_t>(getU32(payload + 8));
    geometry.y = static_cast<int32_t>(getU32(payload + 12));
    geometry.width = static_cast<int32_t>(getU32(payload + 16));
    geometry.height = static_cast<int32_t>(getU32(payload + 20));
    geometry.visible = payload[24] == 1;
    geometry.fullscreen = payload[25] == 1;
    if (geometry.width < 0 || geometry.height < 0)
        return std::nullopt;
    return geometry;
}

struct FrameInfo {
    uint32_t width { 0 };
    uint32_t height { 0 };
    uint32_t stride { 0 };
    double pts { 0 };

    // Plausible and consistent: stride holds a row, the frame fits a slot of
    // `capacity` bytes (or a frame message).
    bool valid(size_t capacity) const
    {
        return width && height && width <= maxFrameWidth && height <= maxFrameHeight
            && stride >= width * 4 && static_cast<uint64_t>(stride) * height <= capacity;
    }
    size_t bytes() const { return static_cast<size_t>(stride) * height; }
};

struct Present {
    uint32_t slot { 0 };
    uint32_t seq { 0 };
    FrameInfo frame;
};

constexpr size_t presentBytes = 28;

inline void appendPresent(std::vector<uint8_t>& out, const Present& present)
{
    uint8_t payload[presentBytes];
    putU32(payload, present.slot);
    putU32(payload + 4, present.seq);
    putU32(payload + 8, present.frame.width);
    putU32(payload + 12, present.frame.height);
    putU32(payload + 16, present.frame.stride);
    putF64(payload + 20, present.frame.pts);
    appendMessage(out, Type::Present, payload, sizeof(payload));
}

inline std::optional<Present> decodePresent(const uint8_t* payload, size_t bytes)
{
    if (bytes != presentBytes)
        return std::nullopt;
    Present present;
    present.slot = getU32(payload);
    present.seq = getU32(payload + 4);
    present.frame.width = getU32(payload + 8);
    present.frame.height = getU32(payload + 12);
    present.frame.stride = getU32(payload + 16);
    present.frame.pts = getF64(payload + 20);
    return present;
}

constexpr size_t frameHeaderBytes = 24;

// Header of a frame message; the pixels follow in the same payload.
inline void encodeFrameHeader(const FrameInfo& frame, uint8_t* out)
{
    putU32(out, frame.width);
    putU32(out + 4, frame.height);
    putU32(out + 8, frame.stride);
    putU32(out + 12, 0);
    putF64(out + 16, frame.pts);
}

// The frame and its pixels (pointing into the payload).
inline std::optional<FrameInfo> decodeFrame(const uint8_t* payload, size_t bytes, const uint8_t** pixels)
{
    if (bytes < frameHeaderBytes)
        return std::nullopt;
    FrameInfo frame;
    frame.width = getU32(payload);
    frame.height = getU32(payload + 4);
    frame.stride = getU32(payload + 8);
    frame.pts = getF64(payload + 16);
    if (!frame.valid(bytes - frameHeaderBytes) || frame.bytes() != bytes - frameHeaderBytes)
        return std::nullopt;
    *pixels = payload + frameHeaderBytes;
    return frame;
}

// MARK: Remote decoding

enum class Codec : uint8_t { Other = 0, H264 = 1, Hevc = 2, Vp9 = 3, Av1 = 4 };

inline const char* codecName(Codec codec)
{
    switch (codec) {
    case Codec::H264:
        return "h264";
    case Codec::Hevc:
        return "hevc";
    case Codec::Vp9:
        return "vp9";
    case Codec::Av1:
        return "av1";
    case Codec::Other:
        break;
    }
    return "other";
}

constexpr uint32_t maxExtradata = 64 * 1024;
constexpr uint32_t maxPacketBytes = 16u << 20;
constexpr size_t maxReasonBytes = 256;

enum class Matrix : uint8_t { Unknown = 0, Bt601 = 1, Bt709 = 2, Bt2020 = 3 };

struct StreamOpen {
    uint32_t stream { 0 };
    Codec codec { Codec::Other };
    Matrix matrix { Matrix::Unknown };
    bool fullRange { false };
    int32_t profile { -1 };
    int32_t level { -1 };
    uint32_t width { 0 };
    uint32_t height { 0 };
    int32_t timeBaseNum { 1 };
    int32_t timeBaseDen { 1000000 };
    std::vector<uint8_t> extradata;
};

constexpr size_t streamOpenHeaderBytes = 32;
// The payload: u32 stream @0, codec @4, matrix @5, range @6, profile @8,
// level @12, width @16, height @20, timebase @24/@28, extradata @32.

inline void appendStreamOpen(std::vector<uint8_t>& out, const StreamOpen& open)
{
    std::vector<uint8_t> payload(streamOpenHeaderBytes + open.extradata.size());
    putU32(payload.data(), open.stream);
    payload[4] = static_cast<uint8_t>(open.codec);
    payload[5] = static_cast<uint8_t>(open.matrix);
    payload[6] = open.fullRange ? 1 : 0;
    putU32(payload.data() + 8, static_cast<uint32_t>(open.profile));
    putU32(payload.data() + 12, static_cast<uint32_t>(open.level));
    putU32(payload.data() + 16, open.width);
    putU32(payload.data() + 20, open.height);
    putU32(payload.data() + 24, static_cast<uint32_t>(open.timeBaseNum));
    putU32(payload.data() + 28, static_cast<uint32_t>(open.timeBaseDen));
    if (!open.extradata.empty())
        memcpy(payload.data() + streamOpenHeaderBytes, open.extradata.data(), open.extradata.size());
    appendMessage(out, Type::StreamOpen, payload.data(), payload.size());
}

inline std::optional<StreamOpen> decodeStreamOpen(const uint8_t* payload, size_t bytes)
{
    if (bytes < streamOpenHeaderBytes || bytes - streamOpenHeaderBytes > maxExtradata)
        return std::nullopt;
    StreamOpen open;
    open.stream = getU32(payload);
    if (payload[4] > static_cast<uint8_t>(Codec::Av1))
        return std::nullopt;
    open.codec = static_cast<Codec>(payload[4]);
    open.matrix = payload[5] <= static_cast<uint8_t>(Matrix::Bt2020) ? static_cast<Matrix>(payload[5]) : Matrix::Unknown;
    open.fullRange = payload[6] == 1;
    open.profile = static_cast<int32_t>(getU32(payload + 8));
    open.level = static_cast<int32_t>(getU32(payload + 12));
    open.width = getU32(payload + 16);
    open.height = getU32(payload + 20);
    open.timeBaseNum = static_cast<int32_t>(getU32(payload + 24));
    open.timeBaseDen = static_cast<int32_t>(getU32(payload + 28));
    if (open.timeBaseNum <= 0 || open.timeBaseDen <= 0)
        return std::nullopt;
    open.extradata.assign(payload + streamOpenHeaderBytes, payload + bytes);
    return open;
}

struct PacketHeader {
    uint32_t stream { 0 };
    uint32_t serial { 0 };
    int64_t pts { 0 };
    int64_t dts { 0 };
    bool keyframe { false };
};

constexpr size_t packetHeaderBytes = 28;

// Header and data in one message, written straight into `out` (no
// intermediate copy of the data).
inline void appendPacket(std::vector<uint8_t>& out, const PacketHeader& packet, const uint8_t* data, size_t bytes)
{
    size_t offset = out.size();
    out.resize(offset + headerSize + packetHeaderBytes + bytes);
    uint8_t* message = out.data() + offset;
    encodeHeader(Type::Packet, static_cast<uint32_t>(packetHeaderBytes + bytes), message);
    uint8_t* header = message + headerSize;
    putU32(header, packet.stream);
    putU32(header + 4, packet.serial);
    putU64(header + 8, static_cast<uint64_t>(packet.pts));
    putU64(header + 16, static_cast<uint64_t>(packet.dts));
    header[24] = packet.keyframe ? 1 : 0;
    header[25] = header[26] = header[27] = 0;
    if (bytes)
        memcpy(header + packetHeaderBytes, data, bytes);
}

// The header; the data is the rest of the payload (`*data`, `*dataBytes`).
inline std::optional<PacketHeader> decodePacket(const uint8_t* payload, size_t bytes, const uint8_t** data, size_t* dataBytes)
{
    if (bytes < packetHeaderBytes || bytes - packetHeaderBytes > maxPacketBytes)
        return std::nullopt;
    PacketHeader packet;
    packet.stream = getU32(payload);
    packet.serial = getU32(payload + 4);
    packet.pts = static_cast<int64_t>(getU64(payload + 8));
    packet.dts = static_cast<int64_t>(getU64(payload + 16));
    packet.keyframe = payload[24] & 1;
    *data = payload + packetHeaderBytes;
    *dataBytes = bytes - packetHeaderBytes;
    return packet;
}

// flush, drain, stream-close (pts unused), show, discard.
struct StreamControl {
    uint32_t stream { 0 };
    uint32_t serial { 0 };
    int64_t pts { 0 };
};

inline void appendStreamControl(std::vector<uint8_t>& out, Type type, const StreamControl& control)
{
    uint8_t payload[16];
    putU32(payload, control.stream);
    putU32(payload + 4, control.serial);
    putU64(payload + 8, static_cast<uint64_t>(control.pts));
    size_t bytes = (type == Type::Show || type == Type::Discard) ? 16 : (type == Type::StreamClose ? 4 : 8);
    appendMessage(out, type, payload, bytes);
}

inline std::optional<StreamControl> decodeStreamControl(Type type, const uint8_t* payload, size_t bytes)
{
    size_t expected = (type == Type::Show || type == Type::Discard) ? 16 : (type == Type::StreamClose ? 4 : 8);
    if (bytes != expected)
        return std::nullopt;
    StreamControl control;
    control.stream = getU32(payload);
    if (bytes >= 8)
        control.serial = getU32(payload + 4);
    if (bytes >= 16)
        control.pts = static_cast<int64_t>(getU64(payload + 8));
    return control;
}

struct StreamOk {
    uint32_t stream { 0 };
    uint8_t mode { 1 };
    uint32_t maxFrames { 0 };
};

inline void appendStreamOk(std::vector<uint8_t>& out, const StreamOk& ok)
{
    uint8_t payload[12] = { };
    putU32(payload, ok.stream);
    payload[4] = ok.mode;
    putU32(payload + 8, ok.maxFrames);
    appendMessage(out, Type::StreamOk, payload, sizeof(payload));
}

inline std::optional<StreamOk> decodeStreamOk(const uint8_t* payload, size_t bytes)
{
    if (bytes != 12)
        return std::nullopt;
    return StreamOk { getU32(payload), payload[4], getU32(payload + 8) };
}

// stream-reject and stream-error: the stream and a reason.
struct StreamFailure {
    uint32_t stream { 0 };
    std::string reason;
};

inline void appendStreamFailure(std::vector<uint8_t>& out, Type type, const StreamFailure& failure)
{
    std::string reason = failure.reason.substr(0, maxReasonBytes);
    std::vector<uint8_t> payload(4 + reason.size());
    putU32(payload.data(), failure.stream);
    memcpy(payload.data() + 4, reason.data(), reason.size());
    appendMessage(out, type, payload.data(), payload.size());
}

inline std::optional<StreamFailure> decodeStreamFailure(const uint8_t* payload, size_t bytes)
{
    if (bytes < 4 || bytes > 4 + maxReasonBytes)
        return std::nullopt;
    StreamFailure failure;
    failure.stream = getU32(payload);
    failure.reason.assign(reinterpret_cast<const char*>(payload + 4), bytes - 4);
    return failure;
}

struct Decoded {
    enum Flags : uint8_t { HasFrame = 1, Drained = 2 };
    uint32_t stream { 0 };
    uint32_t serial { 0 };
    uint32_t consumed { 0 };
    uint8_t flags { 0 };
    uint32_t width { 0 };
    uint32_t height { 0 };
    int64_t pts { 0 };

    bool hasFrame() const { return flags & HasFrame; }
    bool drained() const { return flags & Drained; }
};

constexpr size_t decodedBytes = 32;

inline void appendDecoded(std::vector<uint8_t>& out, const Decoded& decoded)
{
    uint8_t payload[decodedBytes] = { };
    putU32(payload, decoded.stream);
    putU32(payload + 4, decoded.serial);
    putU32(payload + 8, decoded.consumed);
    payload[12] = decoded.flags;
    putU32(payload + 16, decoded.width);
    putU32(payload + 20, decoded.height);
    putU64(payload + 24, static_cast<uint64_t>(decoded.pts));
    appendMessage(out, Type::Decoded, payload, sizeof(payload));
}

inline std::optional<Decoded> decodeDecoded(const uint8_t* payload, size_t bytes)
{
    if (bytes != decodedBytes)
        return std::nullopt;
    Decoded decoded;
    decoded.stream = getU32(payload);
    decoded.serial = getU32(payload + 4);
    decoded.consumed = getU32(payload + 8);
    decoded.flags = payload[12];
    decoded.width = getU32(payload + 16);
    decoded.height = getU32(payload + 20);
    decoded.pts = static_cast<int64_t>(getU64(payload + 24));
    if (decoded.hasFrame() && (!decoded.width || !decoded.height || decoded.width > 8192 || decoded.height > 8192))
        return std::nullopt;
    return decoded;
}

// Shared memory segment: header page, then the slots.
struct ShmLayout {
    static constexpr size_t headerBytes = 4096;
    static constexpr size_t seqOffset = 64;
    static constexpr char magic[9] = "PS5VSHM1";

    uint32_t slots { 0 };
    uint32_t slotBytes { 0 };

    size_t totalBytes() const { return headerBytes + static_cast<size_t>(slots) * slotBytes; }
    size_t slotOffset(uint32_t slot) const { return headerBytes + static_cast<size_t>(slot) * slotBytes; }

    // A slot size for frames up to width x height (BGRA, packed rows),
    // rounded up to whole pages.
    static uint32_t slotBytesFor(uint32_t width, uint32_t height)
    {
        uint64_t bytes = static_cast<uint64_t>(width) * height * 4;
        bytes = (bytes + 4095) & ~uint64_t { 4095 };
        return static_cast<uint32_t>(bytes);
    }

    void writeHeader(uint8_t* base) const
    {
        memset(base, 0, headerBytes);
        memcpy(base, magic, 8);
        putU32(base + 8, slots);
        putU32(base + 12, slotBytes);
    }

    // Whether a mapped segment of `mappedBytes` matches this layout.
    bool matches(const uint8_t* base, size_t mappedBytes) const
    {
        return mappedBytes >= totalBytes() && !memcmp(base, magic, 8) && getU32(base + 8) == slots
            && getU32(base + 12) == slotBytes;
    }

    static std::atomic<uint32_t>& sequence(uint8_t* base, uint32_t slot)
    {
        static_assert(sizeof(std::atomic<uint32_t>) == 4 && std::atomic<uint32_t>::is_always_lock_free);
        return *reinterpret_cast<std::atomic<uint32_t>*>(base + seqOffset + 4 * static_cast<size_t>(slot));
    }
};

// Writer side of a slot's seqlock. Returns the sequence number the frame is
// announced with.
class SlotWriter {
public:
    SlotWriter(uint8_t* base, uint32_t slot)
        : m_sequence(ShmLayout::sequence(base, slot))
        , m_start(m_sequence.load(std::memory_order_relaxed) | 1)
    {
        // Odd: readers that copy the slot now discard their copy.
        m_sequence.store(m_start, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
    }

    // Publishes the slot; the returned (even) value goes into "present".
    uint32_t finish()
    {
        uint32_t done = m_start + 1;
        m_sequence.store(done, std::memory_order_release);
        return done;
    }

private:
    std::atomic<uint32_t>& m_sequence;
    uint32_t m_start;
};

// Reader side: copies `bytes` of the slot into `out` and returns whether the
// copy is exactly the frame announced with `seq` (not torn by a concurrent
// rewrite of the slot).
inline bool readSlot(uint8_t* base, const ShmLayout& layout, uint32_t slot, uint32_t seq, uint8_t* out, size_t bytes)
{
    if (slot >= layout.slots || bytes > layout.slotBytes || (seq & 1))
        return false;
    auto& sequence = ShmLayout::sequence(base, slot);
    if (sequence.load(std::memory_order_acquire) != seq)
        return false;
    memcpy(out, base + layout.slotOffset(slot), bytes);
    std::atomic_thread_fence(std::memory_order_acquire);
    return sequence.load(std::memory_order_relaxed) == seq;
}

// Incremental message parser for either direction (`expectHello` on the
// server). Frame messages are megabytes: writable()/commit() let the caller
// receive straight into the parser's buffer.
class Parser {
public:
    explicit Parser(bool expectHello = false)
        : m_needHello(expectHello)
    {
    }

    // Space for at least `bytes` more input; fill it, then commit() what was
    // written.
    uint8_t* writable(size_t bytes)
    {
        compact();
        if (m_buffer.size() < m_end + bytes)
            m_buffer.resize(m_end + bytes);
        return m_buffer.data() + m_end;
    }

    void commit(size_t bytes) { m_end += bytes; }

    void append(const uint8_t* data, size_t bytes)
    {
        memcpy(writable(bytes), data, bytes);
        commit(bytes);
    }

    enum class Result { NeedMore, Hello, Message, Error };

    struct Message {
        Type type { Type::Close };
        const uint8_t* payload { nullptr };
        uint32_t bytes { 0 };
    };

    // A message's payload stays valid until the next writable()/append().
    Result next()
    {
        if (m_error)
            return Result::Error;
        size_t available = m_end - m_offset;
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

    // Bytes still missing to complete the message being received (at least 1
    // when nothing is known yet), so a frame can be read in one go.
    size_t missingBytes() const
    {
        size_t available = m_end - m_offset;
        if (m_needHello)
            return available < helloSize ? helloSize - available : 0;
        if (available < headerSize)
            return headerSize - available;
        size_t total = headerSize + getU32(m_buffer.data() + m_offset + 4);
        return available < total ? total - available : 0;
    }

    const Hello& hello() const { return m_hello; }
    const Message& message() const { return m_message; }
    size_t pendingBytes() const { return m_end - m_offset; }

private:
    void compact()
    {
        if (m_offset == m_end) {
            m_offset = m_end = 0;
        } else if (m_offset > 0) {
            memmove(m_buffer.data(), m_buffer.data() + m_offset, m_end - m_offset);
            m_end -= m_offset;
            m_offset = 0;
        }
    }

    std::vector<uint8_t> m_buffer;
    size_t m_offset { 0 };
    size_t m_end { 0 };
    bool m_needHello;
    bool m_error { false };
    Hello m_hello;
    Message m_message;
};

} // namespace VideoWire
} // namespace FFmpegMedia
} // namespace WebCore
