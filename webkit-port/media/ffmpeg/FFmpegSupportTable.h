// SPDX-License-Identifier: GPL-3.0-or-later
//
// MIME type / RFC 6381 codec support table of the ffmpeg media engine.
// Engine-independent (no WebCore, WTF or ffmpeg headers) so it can be unit
// tested on the host; MediaPlayerPrivateFFmpeg feeds it the decoders that
// the linked libavcodec actually provides.

#pragma once

#include <algorithm>
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
// The engine reports VP8/VP9 as unavailable unless PROSPEROBROWSER_MEDIA_VP9=1:
// software VP9 is much slower than H.264, and players then pick H.264.
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


// MARK: - Media Source Extensions and content type parameters

// MSE byte stream formats this engine parses (SourceBufferPrivateFFmpeg):
// ISO BMFF (fragmented MP4) and WebM. HLS, MP3, ADTS and the progressive
// aliases (video/x-m4v, video/quicktime, ...) are not MSE byte streams.
inline Container mediaSourceContainerForMIMEType(std::string_view mimeType)
{
    auto lower = lowercase(trim(mimeType));
    if (lower == "video/mp4" || lower == "audio/mp4")
        return Container::MP4;
    if (lower == "video/webm" || lower == "audio/webm")
        return Container::WebM;
    return Container::Unknown;
}

inline bool isVideoFamily(CodecFamily family)
{
    return family == CodecFamily::H264 || family == CodecFamily::HEVC || family == CodecFamily::VP8
        || family == CodecFamily::VP9 || family == CodecFamily::AV1;
}

struct ContentTypeParameter {
    std::string name; // lowercased
    std::string value; // unquoted, case preserved
};

// Splits `type/subtype; name=value; name="quoted, value"` into its MIME type
// and parameters (RFC 7231 media-type syntax, lenient about whitespace).
// Returns the parameters; `mimeType` receives the trimmed type/subtype.
inline std::vector<ContentTypeParameter> parseContentTypeParameters(std::string_view raw, std::string* mimeType = nullptr)
{
    std::vector<ContentTypeParameter> result;
    size_t semicolon = raw.find(';');
    if (mimeType)
        *mimeType = std::string(trim(raw.substr(0, semicolon)));
    size_t position = semicolon == std::string_view::npos ? raw.size() : semicolon + 1;
    while (position < raw.size()) {
        size_t equals = raw.find('=', position);
        size_t nextSemicolon = raw.find(';', position);
        if (equals == std::string_view::npos || (nextSemicolon != std::string_view::npos && nextSemicolon < equals)) {
            // A parameter without a value: skip it.
            if (nextSemicolon == std::string_view::npos)
                break;
            position = nextSemicolon + 1;
            continue;
        }
        ContentTypeParameter parameter;
        parameter.name = lowercase(trim(raw.substr(position, equals - position)));
        size_t valueStart = equals + 1;
        while (valueStart < raw.size() && (raw[valueStart] == ' ' || raw[valueStart] == '\t'))
            ++valueStart;
        size_t end;
        if (valueStart < raw.size() && raw[valueStart] == '"') {
            size_t closing = raw.find('"', valueStart + 1);
            if (closing == std::string_view::npos)
                closing = raw.size();
            parameter.value = std::string(raw.substr(valueStart + 1, closing - valueStart - 1));
            end = raw.find(';', closing);
        } else {
            end = raw.find(';', valueStart);
            parameter.value = std::string(trim(raw.substr(valueStart, (end == std::string_view::npos ? raw.size() : end) - valueStart)));
        }
        if (!parameter.name.empty())
            result.push_back(parameter);
        if (end == std::string_view::npos)
            break;
        position = end + 1;
    }
    return result;
}

inline bool parseDecimal(std::string_view text, double& out)
{
    text = trim(text);
    if (text.empty() || text.size() > 32)
        return false;
    double value = 0;
    double scale = 0;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            if (scale) {
                value += (c - '0') * scale;
                scale /= 10;
            } else
                value = value * 10 + (c - '0');
        } else if (c == '.' && !scale)
            scale = 0.1;
        else
            return false;
    }
    out = value;
    return true;
}

// Limits for the extended isTypeSupported() parameters that YouTube (and
// other Cobalt-style players) use to probe the decoder: anything beyond them
// is "", so a player never picks a stream this build cannot decode in time.
// Video defaults to 1080p (software decoding on the console); the longer
// side may be either dimension, so portrait 1080x1920 fits too.
// PROSPEROBROWSER_MEDIA_MAX_VIDEO=3840x2160 raises it (parseVideoLimits()).
struct VideoLimits {
    double maxLongSide { 1920 };
    double maxShortSide { 1088 };
};
inline constexpr double maxFrameRate = 120;
inline constexpr double maxAudioChannels = 8;

// "WIDTHxHEIGHT" (either order); false when malformed.
inline bool parseVideoLimits(std::string_view text, VideoLimits& limits)
{
    size_t x = text.find_first_of("xX");
    double a = 0;
    double b = 0;
    if (x == std::string_view::npos || !parseDecimal(text.substr(0, x), a) || !parseDecimal(text.substr(x + 1), b) || a < 16 || b < 16)
        return false;
    limits.maxLongSide = std::max(a, b);
    limits.maxShortSide = std::min(a, b);
    return true;
}

// Checks the non-codecs parameters of a content type. IsSupported means "no
// objection" (the codecs decide), NotSupported means some parameter asks for
// something this engine cannot do:
//  - width/height beyond `limits` (1080p by default), framerate above 120,
//    channels above 8, or a value that is not a number;
//  - eotf other than bt709 (smpte2084 and arib-std-b67 are HDR transfer
//    functions; there is no HDR output);
//  - decode-to-texture other than true/false, experimental other than
//    allowed/true/false, tunnelmode other than false (no tunneled playback);
//  - cryptoblockformat with any value (no EME).
// Unknown parameters are ignored.
inline Support evaluateContentTypeParameters(const std::vector<ContentTypeParameter>& parameters, const VideoLimits& limits = { })
{
    double width = 0;
    double height = 0;
    for (auto& parameter : parameters) {
        auto& name = parameter.name;
        auto value = lowercase(trim(parameter.value));
        double number = 0;
        if (name == "width" || name == "height") {
            if (!parseDecimal(value, number) || number > limits.maxLongSide)
                return Support::NotSupported;
            (name == "width" ? width : height) = number;
        } else if (name == "framerate") {
            if (!parseDecimal(value, number) || number > maxFrameRate)
                return Support::NotSupported;
        } else if (name == "channels") {
            if (!parseDecimal(value, number) || number > maxAudioChannels)
                return Support::NotSupported;
        } else if (name == "bitrate") {
            if (!parseDecimal(value, number))
                return Support::NotSupported;
        } else if (name == "eotf") {
            if (value != "bt709")
                return Support::NotSupported;
        } else if (name == "decode-to-texture") {
            if (value != "true" && value != "false")
                return Support::NotSupported;
        } else if (name == "experimental") {
            if (value != "allowed" && value != "true" && value != "false")
                return Support::NotSupported;
        } else if (name == "tunnelmode") {
            if (value != "false")
                return Support::NotSupported;
        } else if (name == "cryptoblockformat")
            return Support::NotSupported;
    }
    if (width > 0 && height > 0 && std::min(width, height) > limits.maxShortSide)
        return Support::NotSupported;
    return Support::IsSupported;
}

inline Support combineSupport(Support a, Support b)
{
    if (a == Support::NotSupported || b == Support::NotSupported)
        return Support::NotSupported;
    if (a == Support::MaybeSupported || b == Support::MaybeSupported)
        return Support::MaybeSupported;
    return Support::IsSupported;
}

// MediaSource.isTypeSupported() / addSourceBuffer() answer for a full content
// type string. No container (MediaPlayer choosing an engine for a MediaSource
// before any type is known) is "maybe", so this engine gets selected.
inline Support evaluateMediaSource(std::string_view rawContentType, const DecoderAvailability& decoders, const VideoLimits& limits = { })
{
    std::string mimeType;
    auto parameters = parseContentTypeParameters(rawContentType, &mimeType);
    if (mimeType.empty())
        return Support::MaybeSupported;
    auto container = mediaSourceContainerForMIMEType(mimeType);
    if (container == Container::Unknown)
        return Support::NotSupported;

    std::vector<std::string> codecs;
    bool hasCodecs = false;
    for (auto& parameter : parameters) {
        if (parameter.name == "codecs") {
            hasCodecs = true;
            codecs = splitCodecs(parameter.value);
        }
    }
    if (hasCodecs && codecs.empty())
        return Support::NotSupported;
    bool isAudioType = startsWith(lowercase(mimeType), "audio/");
    for (auto& codec : codecs) {
        if (isAudioType && isVideoFamily(codecFamily(codec)))
            return Support::NotSupported;
    }
    auto codecSupport = evaluate(mimeType, codecs, decoders);
    return combineSupport(codecSupport, evaluateContentTypeParameters(parameters, limits));
}

} // namespace FFmpegMedia
} // namespace WebCore
