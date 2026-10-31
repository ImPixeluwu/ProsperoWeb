// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIME type / RFC 6381 codec support table of the ffmpeg media engine.
// Engine-independent (no WebCore, WTF or ffmpeg headers) so it can be unit
// tested on the host; MediaPlayerPrivateFFmpeg feeds it the decoders that
// the linked libavcodec actually provides.

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace WebCore {
namespace FFmpegMedia {

enum class Support { NotSupported, MaybeSupported, IsSupported };

// Decoders available at run time. Defaults match the PS5 SDK ffmpeg 7.0.1
// build: native h264/hevc/vp8/vp9/aac/opus/vorbis/mp3/flac/ac3/eac3, but no
// software AV1 decoder (no libdav1d/libaom; ff_av1_decoder is hwaccel-only).
struct DecoderAvailability {
    bool h264 { true };
    bool hevc { true };
    bool vp8 { true };
    bool vp9 { true };
    bool av1 { false };
    bool aac { true };
    bool mp3 { true };
    bool opus { true };
    bool vorbis { true };
    bool flac { true };
    bool ac3 { true };
    bool eac3 { true };
};

enum class Container { Unknown, MP4, WebM, HLS, MPEG, Ogg, Flac, Wav, AAC };

inline char toLowerASCII(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

inline std::string lowercase(std::string_view s)
{
    std::string out(s);
    for (auto& c : out)
        c = toLowerASCII(c);
    return out;
}

inline std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

struct ContainerEntry {
    std::string_view mimeType;
    Container container;
};

// Every MIME type the engine claims. Used both for supportsType() and for
// MediaPlayerFactory::getSupportedTypes().
inline constexpr std::array<ContainerEntry, 22> containerTable { {
    { "video/mp4", Container::MP4 },
    { "audio/mp4", Container::MP4 },
    { "video/x-m4v", Container::MP4 },
    { "audio/x-m4a", Container::MP4 },
    { "video/quicktime", Container::MP4 },
    { "video/webm", Container::WebM },
    { "audio/webm", Container::WebM },
    { "video/x-matroska", Container::WebM },
    { "audio/x-matroska", Container::WebM },
    { "application/vnd.apple.mpegurl", Container::HLS },
    { "application/x-mpegurl", Container::HLS },
    { "audio/mpegurl", Container::HLS },
    { "audio/x-mpegurl", Container::HLS },
    { "audio/mpeg", Container::MPEG },
    { "audio/mp3", Container::MPEG },
    { "audio/ogg", Container::Ogg },
    { "video/ogg", Container::Ogg },
    { "audio/flac", Container::Flac },
    { "audio/x-flac", Container::Flac },
    { "audio/wav", Container::Wav },
    { "audio/x-wav", Container::Wav },
    { "audio/aac", Container::AAC },
} };

inline Container containerForMIMEType(std::string_view mimeType)
{
    auto lower = lowercase(trim(mimeType));
    for (auto& entry : containerTable) {
        if (entry.mimeType == lower)
            return entry.container;
    }
    return Container::Unknown;
}

enum class CodecFamily { Unknown, H264, HEVC, VP8, VP9, AV1, AAC, MP3, Opus, Vorbis, Flac, AC3, EAC3 };

inline bool startsWith(std::string_view s, std::string_view prefix)
{
    return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

// Maps one RFC 6381 codecs-parameter entry (already lowercased) to a family.
inline CodecFamily codecFamily(std::string_view codec)
{
    if (startsWith(codec, "avc1") || startsWith(codec, "avc3"))
        return CodecFamily::H264;
    if (startsWith(codec, "hvc1") || startsWith(codec, "hev1"))
        return CodecFamily::HEVC;
    if (codec == "vp8" || startsWith(codec, "vp8.") || startsWith(codec, "vp08"))
        return CodecFamily::VP8;
    if (codec == "vp9" || startsWith(codec, "vp9.") || startsWith(codec, "vp09"))
        return CodecFamily::VP9;
    if (startsWith(codec, "av01"))
        return CodecFamily::AV1;
    // mp4a.40.x is AAC (2 LC, 5 HE, 29 HE v2, ...) except 34 (MP3); mp4a.69
    // and mp4a.6b are MPEG-1/2 audio.
    if (codec == "mp4a.40.34" || codec == "mp4a.69" || codec == "mp4a.6b" || codec == "mp3")
        return CodecFamily::MP3;
    if (startsWith(codec, "mp4a.40") || codec == "mp4a.66" || codec == "mp4a.67" || codec == "mp4a.68" || codec == "aac")
        return CodecFamily::AAC;
    if (codec == "opus" || codec == "mp4a.ad")
        return CodecFamily::Opus;
    if (codec == "vorbis")
        return CodecFamily::Vorbis;
    if (codec == "flac")
        return CodecFamily::Flac;
    if (codec == "ac-3" || codec == "mp4a.a5")
        return CodecFamily::AC3;
    if (codec == "ec-3" || codec == "mp4a.a6")
        return CodecFamily::EAC3;
    return CodecFamily::Unknown;
}

inline bool decoderAvailable(CodecFamily family, const DecoderAvailability& decoders)
{
    switch (family) {
    case CodecFamily::H264: return decoders.h264;
    case CodecFamily::HEVC: return decoders.hevc;
    case CodecFamily::VP8: return decoders.vp8;
    case CodecFamily::VP9: return decoders.vp9;
    case CodecFamily::AV1: return decoders.av1;
    case CodecFamily::AAC: return decoders.aac;
    case CodecFamily::MP3: return decoders.mp3;
    case CodecFamily::Opus: return decoders.opus;
    case CodecFamily::Vorbis: return decoders.vorbis;
    case CodecFamily::Flac: return decoders.flac;
    case CodecFamily::AC3: return decoders.ac3;
    case CodecFamily::EAC3: return decoders.eac3;
    case CodecFamily::Unknown: return false;
    }
    return false;
}

// Whether a codec family may appear in a container at all (a WebM file
// cannot carry H.264, an MP3 file cannot carry video, ...).
inline bool containerAllows(Container container, CodecFamily family)
{
    switch (container) {
    case Container::MP4:
        return family != CodecFamily::Vorbis;
    case Container::WebM:
        return family == CodecFamily::VP8 || family == CodecFamily::VP9 || family == CodecFamily::AV1
            || family == CodecFamily::Opus || family == CodecFamily::Vorbis;
    case Container::HLS:
        // TS and fMP4 segments: the codecs Safari advertises for HLS plus Opus/FLAC in fMP4.
        return family == CodecFamily::H264 || family == CodecFamily::HEVC || family == CodecFamily::AAC
            || family == CodecFamily::MP3 || family == CodecFamily::AC3 || family == CodecFamily::EAC3
            || family == CodecFamily::Opus || family == CodecFamily::Flac || family == CodecFamily::VP9 || family == CodecFamily::AV1;
    case Container::MPEG:
        return family == CodecFamily::MP3;
    case Container::Ogg:
        return family == CodecFamily::Opus || family == CodecFamily::Vorbis || family == CodecFamily::Flac;
    case Container::Flac:
        return family == CodecFamily::Flac;
    case Container::Wav:
        return false; // PCM has no RFC 6381 name; only "1" is used in practice.
    case Container::AAC:
        return family == CodecFamily::AAC;
    case Container::Unknown:
        return false;
    }
    return false;
}

// Splits a codecs parameter value ("avc1.64001f, mp4a.40.2") into entries.
inline std::vector<std::string> splitCodecs(std::string_view codecs)
{
    std::vector<std::string> result;
    size_t start = 0;
    while (start <= codecs.size()) {
        size_t comma = codecs.find(',', start);
        if (comma == std::string_view::npos)
            comma = codecs.size();
        auto item = trim(codecs.substr(start, comma - start));
        if (!item.empty())
            result.push_back(lowercase(item));
        start = comma + 1;
    }
    return result;
}

// HTML canPlayType()/isTypeSupported() semantics:
//  - unknown container: NotSupported ("");
//  - known container, no codecs parameter: MaybeSupported ("maybe");
//  - every listed codec known and decodable in this container: IsSupported ("probably");
//  - a codec known to be undecodable or impossible in the container: NotSupported;
//  - otherwise (a codec string we do not recognise): MaybeSupported.
inline Support evaluate(std::string_view mimeType, const std::vector<std::string>& codecs, const DecoderAvailability& decoders)
{
    auto container = containerForMIMEType(mimeType);
    if (container == Container::Unknown)
        return Support::NotSupported;
    if (codecs.empty())
        return Support::MaybeSupported;

    Support result = Support::IsSupported;
    for (auto& rawCodec : codecs) {
        auto codec = lowercase(trim(rawCodec));
        if (container == Container::Wav && (codec == "1" || codec == "3"))
            continue;
        auto family = codecFamily(codec);
        if (family == CodecFamily::Unknown) {
            result = Support::MaybeSupported;
            continue;
        }
        if (!containerAllows(container, family) || !decoderAvailable(family, decoders))
            return Support::NotSupported;
    }
    return result;
}

} // namespace FFmpegMedia
} // namespace WebCore
