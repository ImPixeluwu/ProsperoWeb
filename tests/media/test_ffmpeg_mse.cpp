// SPDX-License-Identifier: GPL-3.0-or-later
//
// Host tests for the engine-independent parts of the ffmpeg MSE backend:
// isTypeSupported() answers (FFmpegSupportTable.h), the byte queue and
// segment scanner (mse/FFmpegMSEParsing.h) and the player policy
// (mse/FFmpegMSEPolicy.h).
#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "FFmpegMSEParsing.h"
#include "FFmpegMSEPolicy.h"
#include "FFmpegSupportTable.h"

using namespace WebCore::FFmpegMedia;

namespace {

Support mse(std::string_view type, DecoderAvailability decoders = { })
{
    return evaluateMediaSource(type, decoders);
}

using Bytes = std::vector<uint8_t>;

void put32(Bytes& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

// An MP4 box of `type` with `payload` filler bytes.
Bytes box(const char (&type)[5], size_t payload, uint8_t fill = 0)
{
    Bytes out;
    put32(out, static_cast<uint32_t>(8 + payload));
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>(type[i]));
    out.insert(out.end(), payload, fill);
    return out;
}

Bytes concat(std::initializer_list<Bytes> parts)
{
    Bytes out;
    for (auto& part : parts)
        out.insert(out.end(), part.begin(), part.end());
    return out;
}

// A WebM element with a 4-byte ID or 1-byte ID and a 1- or 8-byte size.
Bytes element(uint32_t id, size_t payload, bool unknownSize = false, uint8_t fill = 0)
{
    Bytes out;
    if (id > 0xFFFFFF)
        put32(out, id);
    else if (id > 0xFFFF) {
        out.push_back(static_cast<uint8_t>(id >> 16));
        out.push_back(static_cast<uint8_t>(id >> 8));
        out.push_back(static_cast<uint8_t>(id));
    } else if (id > 0xFF) {
        out.push_back(static_cast<uint8_t>(id >> 8));
        out.push_back(static_cast<uint8_t>(id));
    } else
        out.push_back(static_cast<uint8_t>(id));
    if (unknownSize) {
        out.push_back(0x01);
        out.insert(out.end(), 7, 0xFF);
    } else if (payload < 127)
        out.push_back(static_cast<uint8_t>(0x80 | payload));
    else {
        out.push_back(0x01);
        for (int shift = 48; shift >= 0; shift -= 8)
            out.push_back(static_cast<uint8_t>(payload >> shift));
    }
    out.insert(out.end(), payload, fill);
    return out;
}

struct ScanResult {
    bool ok { true };
    std::vector<ByteStreamSegment> segments;
};

ScanResult scan(SegmentScanner& scanner, const Bytes& bytes, size_t chunk = 0)
{
    ScanResult result;
    if (!chunk)
        chunk = bytes.size() ? bytes.size() : 1;
    for (size_t offset = 0; offset < bytes.size() && result.ok; offset += chunk)
        result.ok = scanner.append(bytes.data() + offset, std::min(chunk, bytes.size() - offset), result.segments);
    return result;
}

size_t countKind(const ScanResult& result, ByteStreamSegment::Kind kind)
{
    size_t count = 0;
    for (auto& segment : result.segments)
        count += segment.kind == kind;
    return count;
}

size_t mediaBytes(const ScanResult& result)
{
    size_t total = 0;
    for (auto& segment : result.segments) {
        if (segment.kind == ByteStreamSegment::Kind::Media && !segment.partial)
            total += segment.bytes.size();
    }
    return total;
}

Bytes completeMedia(const ScanResult& result)
{
    Bytes out;
    for (auto& segment : result.segments) {
        if (segment.kind == ByteStreamSegment::Kind::Media && !segment.partial)
            out.insert(out.end(), segment.bytes.begin(), segment.bytes.end());
    }
    return out;
}

// An MP4 box with a 64-bit size (size field 1).
Bytes largeBox(const char (&type)[5], size_t payload, uint8_t fill = 0)
{
    Bytes out;
    put32(out, 1);
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>(type[i]));
    put32(out, 0);
    put32(out, static_cast<uint32_t>(16 + payload));
    out.insert(out.end(), payload, fill);
    return out;
}

} // namespace

TEST_SUITE("media_mse_support") {
  TEST_CASE("YouTube-style probes") {
    // Plain H.264/AAC and VP9/Opus: supported (the SDK has the decoders).
    CHECK(mse("video/mp4; codecs=\"avc1.4d401e\"") == Support::IsSupported);
    CHECK(mse("video/mp4; codecs=\"avc1.64002a\"; width=1920; height=1080; framerate=60") == Support::IsSupported);
    CHECK(mse("audio/mp4; codecs=\"mp4a.40.2\"") == Support::IsSupported);
    CHECK(mse("audio/mp4; codecs=\"mp4a.40.5\"; channels=2") == Support::IsSupported);
    CHECK(mse("video/webm; codecs=\"vp9\"") == Support::IsSupported);
    CHECK(mse("video/webm; codecs=\"vp09.00.51.08.01.01.01.01.00\"; eotf=bt709") == Support::IsSupported);
    CHECK(mse("audio/webm; codecs=\"opus\"") == Support::IsSupported);
    // Absurd sizes and rates are refused, so the player believes the answers.
    CHECK(mse("video/webm; codecs=\"vp9\"; width=99999") == Support::NotSupported);
    CHECK(mse("video/mp4; codecs=\"avc1.4d401e\"; height=99999") == Support::NotSupported);
    // 1080p by default (landscape or portrait); PROSPEROWEB_MEDIA_MAX_VIDEO raises it.
    CHECK(mse("video/webm; codecs=\"vp9\"; width=1920; height=1080") == Support::IsSupported);
    CHECK(mse("video/mp4; codecs=\"avc1.640028\"; width=1080; height=1920") == Support::IsSupported);
    CHECK(mse("video/webm; codecs=\"vp9\"; width=3840; height=2160") == Support::NotSupported);
    CHECK(mse("video/mp4; codecs=\"avc1.640033\"; width=2560") == Support::NotSupported);
    CHECK(mse("video/mp4; codecs=\"avc1.640033\"; width=2048; height=1536") == Support::NotSupported);
    VideoLimits uhd;
    REQUIRE(parseVideoLimits("3840x2160", uhd));
    CHECK(uhd.maxLongSide == 3840);
    CHECK(uhd.maxShortSide == 2160);
    CHECK(evaluateMediaSource("video/webm; codecs=\"vp9\"; width=3840; height=2160", { }, uhd) == Support::IsSupported);
    CHECK(evaluateMediaSource("video/webm; codecs=\"vp9\"; width=4096; height=4096", { }, uhd) == Support::NotSupported);
    CHECK_FALSE(parseVideoLimits("big", uhd));
    CHECK_FALSE(parseVideoLimits("8x8", uhd));
    CHECK(mse("video/webm; codecs=\"vp9\"; framerate=9999") == Support::NotSupported);
    CHECK(mse("video/webm; codecs=\"vp9\"; framerate=abc") == Support::NotSupported);
    CHECK(mse("audio/webm; codecs=\"opus\"; channels=99") == Support::NotSupported);
    // HDR transfer functions and made-up ones.
    CHECK(mse("video/webm; codecs=\"vp09.02.51.10.01.09.16.09.00\"; eotf=smpte2084") == Support::NotSupported);
    CHECK(mse("video/webm; codecs=\"vp9\"; eotf=arib-std-b67") == Support::NotSupported);
    CHECK(mse("video/webm; codecs=\"vp9\"; eotf=catavision") == Support::NotSupported);
    // Cobalt-specific switches.
    CHECK(mse("video/webm; codecs=\"vp9\"; decode-to-texture=nope") == Support::NotSupported);
    CHECK(mse("video/webm; codecs=\"vp9\"; decode-to-texture=false") == Support::IsSupported);
    CHECK(mse("video/webm; codecs=\"vp9\"; experimental=invalid") == Support::NotSupported);
    CHECK(mse("video/mp4; codecs=\"avc1.4d401e\"; tunnelmode=true") == Support::NotSupported);
    CHECK(mse("video/mp4; codecs=\"avc1.4d401e\"; tunnelmode=false") == Support::IsSupported);
    // No EME.
    CHECK(mse("video/mp4; codecs=\"avc1.4d401e\"; cryptoblockformat=subsample") == Support::NotSupported);
  }

  TEST_CASE("VP9 off: players fall back to H.264") {
    DecoderAvailability noVP9;
    noVP9.vp8 = false;
    noVP9.vp9 = false;
    CHECK(mse("video/webm; codecs=\"vp09.00.51.08.01.01.01.01.00\"", noVP9) == Support::NotSupported);
    CHECK(mse("video/webm; codecs=\"vp9\"", noVP9) == Support::NotSupported);
    CHECK(mse("video/webm; codecs=\"vp8\"", noVP9) == Support::NotSupported);
    CHECK(mse("video/mp4; codecs=\"vp09.00.10.08\"", noVP9) == Support::NotSupported);
    CHECK(mse("video/mp4; codecs=\"avc1.4d401f\"", noVP9) == Support::IsSupported);
    CHECK(mse("audio/webm; codecs=\"opus\"", noVP9) == Support::IsSupported);
    CHECK(mse("audio/mp4; codecs=\"mp4a.40.2\"", noVP9) == Support::IsSupported);
  }

  TEST_CASE("codecs and containers") {
    CHECK(mse("video/mp4; codecs=\"av01.0.05M.08\"") == Support::NotSupported);
    DecoderAvailability withDav1d;
    withDav1d.av1 = true;
    CHECK(mse("video/mp4; codecs=\"av01.0.05M.08\"", withDav1d) == Support::IsSupported);
    DecoderAvailability noHevc;
    noHevc.hevc = false;
    CHECK(mse("video/mp4; codecs=\"hvc1.1.6.L93.B0\"") == Support::IsSupported);
    CHECK(mse("video/mp4; codecs=\"hev1.1.6.L93.B0\"", noHevc) == Support::NotSupported);
    CHECK(mse("video/mp4; codecs=\"avc1.4d401e, mp4a.40.2\"") == Support::IsSupported);
    CHECK(mse("video/webm; codecs=\"avc1.4d401e\"") == Support::NotSupported);
    CHECK(mse("audio/mp4; codecs=\"avc1.4d401e\"") == Support::NotSupported);
    CHECK(mse("video/mp4; codecs=\"foo.bar\"") == Support::MaybeSupported);
    CHECK(mse("video/mp4") == Support::MaybeSupported);
    CHECK(mse("video/mp4; codecs=\"\"") == Support::NotSupported);
    // Not MSE byte streams.
    CHECK(mse("application/vnd.apple.mpegurl") == Support::NotSupported);
    CHECK(mse("video/x-m4v; codecs=\"avc1.4d401e\"") == Support::NotSupported);
    CHECK(mse("audio/mpeg") == Support::NotSupported);
    // Engine selection for a MediaSource happens before any type is known.
    CHECK(mse("") == Support::MaybeSupported);
  }

  TEST_CASE("parameter parsing") {
    std::string mime;
    auto parameters = parseContentTypeParameters("Video/MP4 ; Codecs = \"avc1.4d401e, mp4a.40.2\" ;width=640;flag; eotf=BT709", &mime);
    CHECK(mime == "Video/MP4");
    REQUIRE(parameters.size() == 3);
    CHECK(parameters[0].name == "codecs");
    CHECK(parameters[0].value == "avc1.4d401e, mp4a.40.2");
    CHECK(parameters[1].name == "width");
    CHECK(parameters[1].value == "640");
    CHECK(parameters[2].value == "BT709");
    CHECK(evaluateContentTypeParameters(parameters) == Support::IsSupported);
    CHECK(mse("VIDEO/WEBM; CODECS=VP9; EOTF=BT709") == Support::IsSupported);
    double number = 0;
    CHECK(parseDecimal("29.97", number));
    CHECK(number == doctest::Approx(29.97));
    CHECK_FALSE(parseDecimal("1e9", number));
    CHECK_FALSE(parseDecimal("", number));
  }
}

TEST_SUITE("media_mse_bytes") {
  TEST_CASE("byte queue reads, seeks and consumes like a file") {
    ByteQueue queue;
    const uint8_t data[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    queue.append(data, 5);
    queue.append(data + 5, 3);
    CHECK(queue.size() == 8);
    uint8_t out[8] = { };
    CHECK(queue.read(out, 3) == 3);
    CHECK(out[2] == 3);
    CHECK(queue.position() == 3);
    CHECK(queue.seek(-1, ByteQueue::Whence::End) == 7);
    CHECK(queue.read(out, 8) == 1);
    CHECK(out[0] == 8);
    CHECK(queue.read(out, 8) == 0); // end of data
    CHECK(queue.seek(9, ByteQueue::Whence::Set) == -1);
    CHECK(queue.position() == 8);
    CHECK(queue.seek(-8, ByteQueue::Whence::Current) == 0);
    CHECK(queue.seek(-1, ByteQueue::Whence::Current) == -1);
    queue.seek(4, ByteQueue::Whence::Set);
    queue.consume(2);
    CHECK(queue.size() == 6);
    CHECK(queue.position() == 2);
    CHECK(queue.data()[0] == 3);
    queue.consume(100);
    CHECK(queue.empty());
    CHECK(queue.position() == 0);
  }

  TEST_CASE("MP4: init segment then media in arbitrary chunks") {
    auto init = concat({ box("ftyp", 16), box("free", 4), box("moov", 600) });
    auto media = concat({ box("styp", 16), box("sidx", 36), box("moof", 200, 1), box("mdat", 5000, 2) });
    for (size_t chunk : { size_t(0), size_t(1), size_t(7), size_t(1000) }) {
        SegmentScanner scanner(ByteStreamFormat::MP4);
        auto first = scan(scanner, init, chunk);
        REQUIRE(first.ok);
        REQUIRE(countKind(first, ByteStreamSegment::Kind::Initialization) == 1);
        // ftyp + moov only (free dropped).
        CHECK(first.segments[0].bytes.size() == 24 + 608);
        CHECK(scanner.hasInitializationSegment());
        auto second = scan(scanner, media, chunk);
        REQUIRE(second.ok);
        // moof + mdat only (styp/sidx dropped).
        CHECK(mediaBytes(second) == 208 + 5008);
        CHECK(scanner.pendingBytes() == 0);
    }
  }

  TEST_CASE("MP4: partial fragment waits, abort keeps the init segment") {
    SegmentScanner scanner(ByteStreamFormat::MP4);
    REQUIRE(scan(scanner, concat({ box("ftyp", 8), box("moov", 100) })).ok);
    auto fragment = concat({ box("moof", 50), box("mdat", 400) });
    Bytes head(fragment.begin(), fragment.begin() + 300);
    auto partial = scan(scanner, head);
    CHECK(partial.ok);
    CHECK(partial.segments.empty());
    CHECK(scanner.pendingBytes() == 300);
    scanner.reset();
    CHECK(scanner.pendingBytes() == 0);
    CHECK(scanner.hasInitializationSegment());
    auto again = scan(scanner, fragment);
    CHECK(again.ok);
    CHECK(mediaBytes(again) == fragment.size());
  }

  TEST_CASE("MP4: errors") {
    SegmentScanner early(ByteStreamFormat::MP4);
    CHECK_FALSE(scan(early, concat({ box("moof", 10), box("mdat", 10) })).ok);
    SegmentScanner broken(ByteStreamFormat::MP4);
    Bytes tiny { 0, 0, 0, 4, 'f', 't', 'y', 'p' };
    CHECK_FALSE(scan(broken, tiny).ok);
    CHECK_FALSE(broken.error().empty());
    // A second init segment (quality switch) is emitted again.
    SegmentScanner twice(ByteStreamFormat::MP4);
    auto init = concat({ box("ftyp", 8), box("moov", 40) });
    auto media = concat({ box("moof", 8), box("mdat", 8) });
    auto result = scan(twice, concat({ init, media, init, media }));
    CHECK(result.ok);
    CHECK(countKind(result, ByteStreamSegment::Kind::Initialization) == 2);
    CHECK(countKind(result, ByteStreamSegment::Kind::Media) == 2);
  }

  TEST_CASE("WebM: init, known and unknown-size clusters, cues skipped") {
    auto ebml = element(WebMID::EBML, 20);
    auto segmentHeader = element(WebMID::Segment, 0, true);
    segmentHeader.resize(12); // header only: the Segment is entered
    auto init = concat({ ebml, segmentHeader, element(WebMID::SeekHead, 30), element(WebMID::Void, 10),
        element(WebMID::Info, 40), element(WebMID::Tracks, 60) });
    auto cues = element(WebMID::Cues, 300);
    auto cluster = concat({ element(WebMID::Cluster, 0, true), element(WebMID::Timecode, 2), element(WebMID::SimpleBlock, 500, false, 7) });
    auto knownCluster = element(WebMID::Cluster, 900);
    for (size_t chunk : { size_t(0), size_t(3), size_t(64) }) {
        SegmentScanner scanner(ByteStreamFormat::WebM);
        auto result = scan(scanner, concat({ init, cues, knownCluster, cluster, knownCluster }), chunk);
        REQUIRE(result.ok);
        REQUIRE(countKind(result, ByteStreamSegment::Kind::Initialization) == 1);
        // EBML + Segment header + Info + Tracks (SeekHead and Void dropped).
        CHECK(result.segments[0].bytes.size() == ebml.size() + 12 + element(WebMID::Info, 40).size() + element(WebMID::Tracks, 60).size());
        // The unknown-size cluster ends where the next Cluster starts.
        CHECK(mediaBytes(result) == knownCluster.size() * 2 + cluster.size());
        CHECK(scanner.pendingBytes() == 0);
    }
  }

  TEST_CASE("WebM: unknown-size cluster at the end waits for more") {
    SegmentScanner scanner(ByteStreamFormat::WebM);
    auto segmentHeader = element(WebMID::Segment, 0, true);
    segmentHeader.resize(12);
    REQUIRE(scan(scanner, concat({ element(WebMID::EBML, 4), segmentHeader, element(WebMID::Info, 4), element(WebMID::Tracks, 4) })).ok);
    auto cluster = concat({ element(WebMID::Cluster, 0, true), element(WebMID::Timecode, 2), element(WebMID::SimpleBlock, 50) });
    auto open = scan(scanner, cluster);
    CHECK(open.ok);
    CHECK(mediaBytes(open) == 0);
    auto closed = scan(scanner, element(WebMID::Cluster, 10));
    CHECK(closed.ok);
    CHECK(mediaBytes(closed) == cluster.size() + element(WebMID::Cluster, 10).size());
    SegmentScanner early(ByteStreamFormat::WebM);
    CHECK_FALSE(scan(early, element(WebMID::Cluster, 10)).ok);
  }

  TEST_CASE("MP4: a fragment streamed in small appends is exposed as it grows") {
    auto init = concat({ box("ftyp", 8), box("moov", 100) });
    // moof + 64-bit-size mdat, then a regular pair, with the boxes a DASH /
    // YouTube stream carries between fragments.
    auto first = concat({ box("moof", 120, 1), largeBox("mdat", 40000, 2) });
    auto second = concat({ box("moof", 80, 3), box("mdat", 9000, 4) });
    auto stream = concat({ box("styp", 16), box("sidx", 40), box("emsg", 70), first, box("prft", 24), box("free", 20000), second });
    SegmentScanner scanner(ByteStreamFormat::MP4, 4096);
    REQUIRE(scan(scanner, init).ok);
    ScanResult result;
    size_t maxPending = 0;
    unsigned completedAfter = 0;
    for (size_t offset = 0; offset < stream.size(); offset += 1000) {
        REQUIRE(scanner.append(stream.data() + offset, std::min<size_t>(1000, stream.size() - offset), result.segments));
        maxPending = std::max(maxPending, scanner.pendingBytes());
        completedAfter = std::max(completedAfter, scanner.lastFragmentAppends());
    }
    // Both fragments exactly once, nothing else, nothing left over.
    CHECK(completeMedia(result) == concat({ first, second }));
    CHECK(scanner.pendingBytes() == 0);
    // The 20 KB free box is skipped, never buffered.
    CHECK(maxPending <= first.size());
    CHECK(completedAfter >= 40);
    // Partial segments: growing prefixes of the fragment they belong to,
    // and the complete segment names the fragment it continues.
    size_t partials = 0, previous = 0;
    uint64_t firstFragment = 0;
    bool continued = false;
    for (auto& segment : result.segments) {
        if (segment.partial && (!firstFragment || segment.fragment == firstFragment)) {
            ++partials;
            REQUIRE(segment.bytes.size() <= first.size());
            CHECK(std::equal(segment.bytes.begin(), segment.bytes.end(), first.begin()));
            CHECK(segment.bytes.size() >= previous + 4096);
            previous = segment.bytes.size();
            firstFragment = segment.fragment;
        } else if (segment.partial) {
            CHECK(segment.fragment > firstFragment);
            CHECK(std::equal(segment.bytes.begin(), segment.bytes.end(), second.begin()));
        } else if (segment.kind == ByteStreamSegment::Kind::Media && segment.bytes.size() >= first.size())
            continued = firstFragment && segment.fragment == firstFragment;
    }
    CHECK(partials >= 3);
    CHECK(partials <= 12); // the step grows with the fragment
    CHECK(continued);
  }

  TEST_CASE("MP4: a moof without its mdat is superseded, mdat without moof skipped") {
    SegmentScanner scanner(ByteStreamFormat::MP4);
    REQUIRE(scan(scanner, concat({ box("ftyp", 8), box("moov", 100) })).ok);
    auto pair = concat({ box("moof", 30, 5), box("mdat", 300, 6) });
    auto result = scan(scanner, concat({ box("mdat", 500), box("moof", 40), pair }), 7);
    CHECK(result.ok);
    CHECK(completeMedia(result) == pair);
    CHECK(scanner.pendingBytes() == 0);
    SegmentScanner toEnd(ByteStreamFormat::MP4);
    REQUIRE(scan(toEnd, concat({ box("ftyp", 8), box("moov", 100) })).ok);
    Bytes open { 0, 0, 0, 0, 'm', 'd', 'a', 't' };
    CHECK_FALSE(scan(toEnd, open).ok);
  }

  TEST_CASE("WebM: partial clusters stop at complete children") {
    auto segmentHeader = element(WebMID::Segment, 0, true);
    segmentHeader.resize(12);
    auto init = concat({ element(WebMID::EBML, 4), segmentHeader, element(WebMID::Info, 4), element(WebMID::Tracks, 4) });
    Bytes blocks;
    for (int i = 0; i < 40; ++i) {
        auto block = element(WebMID::SimpleBlock, 300, false, static_cast<uint8_t>(i));
        blocks.insert(blocks.end(), block.begin(), block.end());
    }
    auto children = concat({ element(WebMID::Timecode, 2), blocks });
    auto unknown = concat({ element(WebMID::Cluster, 0, true), children });
    auto knownHeader = element(WebMID::Cluster, children.size());
    knownHeader.resize(12); // ID + 8-byte size, without the filler
    auto known = concat({ knownHeader, children });
    for (auto* cluster : { &unknown, &known }) {
        SegmentScanner scanner(ByteStreamFormat::WebM, 1024);
        REQUIRE(scan(scanner, init).ok);
        auto stream = concat({ *cluster, element(WebMID::Cues, 10) });
        auto result = scan(scanner, stream, 97);
        REQUIRE(result.ok);
        CHECK(completeMedia(result) == *cluster);
        size_t partials = 0;
        for (auto& segment : result.segments) {
            if (!segment.partial)
                continue;
            ++partials;
            // Header + Timecode + whole SimpleBlocks only.
            size_t blockBytes = segment.bytes.size() - (cluster->size() - children.size()) - element(WebMID::Timecode, 2).size();
            CHECK(blockBytes % element(WebMID::SimpleBlock, 300).size() == 0);
        }
        CHECK(partials >= 3);
        CHECK(scanner.pendingBytes() == 0);
    }
  }

  TEST_CASE("EBML header parsing") {
    EBMLHeader header;
    const uint8_t cluster[] = { 0x1F, 0x43, 0xB6, 0x75, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    REQUIRE(readEBMLHeader(cluster, sizeof(cluster), header) == ParseStatus::Ok);
    CHECK(header.id == WebMID::Cluster);
    CHECK(header.unknownSize);
    CHECK(header.headerLength == 12);
    const uint8_t block[] = { 0xA3, 0x42, 0x10 };
    REQUIRE(readEBMLHeader(block, sizeof(block), header) == ParseStatus::Ok);
    CHECK(header.size == 0x210);
    CHECK(readEBMLHeader(block, 1, header) == ParseStatus::NeedMoreData);
    const uint8_t invalid[] = { 0x00, 0x80 };
    CHECK(readEBMLHeader(invalid, sizeof(invalid), header) == ParseStatus::Invalid);
  }
}

TEST_SUITE("media_mse_time") {
  TEST_CASE("timestamps keep their timebase exactly") {
    auto video = timeValueFromTimestamp(90000 * 5 + 3003, 1, 90000);
    CHECK(video.valid);
    CHECK(video.value == 453003);
    CHECK(video.timescale == 90000);
    auto webm = timeValueFromTimestamp(-7, 1, 1000);
    CHECK(webm.valid);
    CHECK(webm.value == -7);
    auto scaled = timeValueFromTimestamp(30, 1001, 30000);
    CHECK(scaled.value == 30030);
    CHECK_FALSE(timeValueFromTimestamp(1, 0, 1000).valid);
    CHECK_FALSE(timeValueFromTimestamp(std::numeric_limits<int64_t>::max() / 2, 4, 1000).valid);
  }

  TEST_CASE("microsecond conversion and timestampOffset") {
    CHECK(microsecondsFromTimeValue(3003, 90000) == 33367); // 33366.67 us
    CHECK(microsecondsFromTimeValue(1, 3) == 333333);
    CHECK(microsecondsFromTimeValue(-1, 3) == -333333);
    CHECK(microsecondsFromTimeValue(2048, 48000) == 42667);
    CHECK(microsecondsFromTimeValue(5, 0) == 0);
    // A sample at 4.0 s (1/30 timebase) with timestampOffset 10.5 s: the
    // generic code adds MediaTimes; the queue gets microseconds.
    auto sample = timeValueFromTimestamp(120, 1, 30);
    CHECK(microsecondsFromTimeValue(sample.value, sample.timescale) + microsecondsFromSeconds(10.5) == 14500000);
    CHECK(microsecondsFromSeconds(-0.0000005) == -1);
    CHECK(microsecondsFromSeconds(0.0000004) == 0);
  }

  TEST_CASE("missing durations are filled from the next sample") {
    std::vector<SampleTiming> samples { { 0, 0, 0 }, { 40, 40, 0 }, { 80, 80, 25 }, { 120, 120, 0 } };
    fillMissingDurations(samples, 33);
    CHECK(samples[0].duration == 40);
    CHECK(samples[1].duration == 40);
    CHECK(samples[2].duration == 25);
    CHECK(samples[3].duration == 25); // last: previous duration
    std::vector<SampleTiming> single { { 0, 0, 0 } };
    fillMissingDurations(single, 33);
    CHECK(single[0].duration == 33);
    std::vector<SampleTiming> sameDts { { 10, 10, 0 }, { 10, 20, 0 } };
    fillMissingDurations(sameDts, 5);
    CHECK(sameDts[0].duration == 5);
  }

  TEST_CASE("track four-character codes") {
    CHECK(fourCCForCodecName("h264") == "avc1");
    CHECK(fourCCForCodecName("vp9") == "vp09");
    CHECK(fourCCForCodecName("aac") == "mp4a");
    CHECK(fourCCForCodecName("opus") == "opus");
    CHECK(fourCCForCodecName("hevc") == "hvc1");
    CHECK(fourCCForCodecName("wmv3") == "????");
  }
}

TEST_SUITE("media_mse_policy") {
  TEST_CASE("decoder queue feeding") {
    QueueLevel empty { true, 0, 0 };
    QueueLevel few { true, 5, 3.0 };
    QueueLevel half { true, 30, 1.0 };
    QueueLevel full { true, 60, 1.6 };
    QueueLevel many { true, 240, 0.5 };
    CHECK(mseQueueAcceptsMore(empty));
    CHECK(mseQueueAcceptsMore(few));
    CHECK(mseQueueAcceptsMore(half));
    CHECK_FALSE(mseQueueAcceptsMore(full));
    CHECK_FALSE(mseQueueAcceptsMore(many));
    CHECK_FALSE(mseQueueShouldRefill(half));
    CHECK(mseQueueShouldRefill(QueueLevel { true, 20, 0.5 }));
    CHECK(mseQueueShouldRefill(few));
    CHECK(mseBufferQuota(true) == 64u << 20);
    CHECK(mseBufferQuota(false) == 8u << 20);
  }

  TEST_CASE("readyState gating") {
    MSEReadyInputs in;
    in.fromMediaSource = ReadyLevel::HaveEnoughData;
    in.videoRendered = true;
    in.videoFrameShown = false;
    CHECK(mseReadyLevel(in) == ReadyLevel::HaveMetadata);
    in.videoFrameShown = true;
    CHECK(mseReadyLevel(in) == ReadyLevel::HaveEnoughData);
    in.seeking = true;
    CHECK(mseReadyLevel(in) == ReadyLevel::HaveMetadata);
    in.seeking = false;
    in.fromMediaSource = ReadyLevel::HaveMetadata;
    CHECK(mseReadyLevel(in) == ReadyLevel::HaveMetadata);
    in.fromMediaSource = ReadyLevel::HaveNothing;
    CHECK(mseReadyLevel(in) == ReadyLevel::HaveNothing);
    MSEReadyInputs audioOnly;
    audioOnly.fromMediaSource = ReadyLevel::HaveFutureData;
    audioOnly.audioReady = false;
    CHECK(mseReadyLevel(audioOnly) == ReadyLevel::HaveMetadata);
    audioOnly.audioReady = true;
    CHECK(mseReadyLevel(audioOnly) == ReadyLevel::HaveFutureData);
  }

  TEST_CASE("readyState from buffered ranges") {
    TimeRangeSet buffered;
    buffered.add(0, 10);
    buffered.add(20, 22);
    double inf = std::numeric_limits<double>::infinity();
    CHECK(readyLevelFromBuffered(buffered, 0, inf, false, false) == ReadyLevel::HaveNothing);
    CHECK(readyLevelFromBuffered(buffered, 5, inf, false, true) == ReadyLevel::HaveEnoughData);
    CHECK(readyLevelFromBuffered(buffered, 9, inf, false, true) == ReadyLevel::HaveFutureData);
    CHECK(readyLevelFromBuffered(buffered, 9.9, inf, false, true) == ReadyLevel::HaveCurrentData);
    CHECK(readyLevelFromBuffered(buffered, 15, inf, false, true) == ReadyLevel::HaveMetadata);
    CHECK(readyLevelFromBuffered(buffered, 21, 22, false, true) == ReadyLevel::HaveFutureData);
    CHECK(readyLevelFromBuffered(buffered, 21.9, 22, true, true) == ReadyLevel::HaveEnoughData);
    CHECK(readyLevelFromBuffered(buffered, 22, 22, true, true) == ReadyLevel::HaveEnoughData);
  }

  TEST_CASE("seek phases") {
    MSESeekInputs in;
    in.dataAvailable = false;
    CHECK(mseSeekPhase(in) == MSESeekPhase::WaitingForData);
    in.secondsSinceReenqueue = 100; // no data: never times out, the page must append
    CHECK(mseSeekPhase(in) == MSESeekPhase::WaitingForData);
    in.dataAvailable = true;
    in.secondsSinceReenqueue = 0.1;
    in.videoReady = false;
    CHECK(mseSeekPhase(in) == MSESeekPhase::WaitingForFrames);
    in.videoReady = true;
    in.audioReady = false;
    CHECK(mseSeekPhase(in) == MSESeekPhase::WaitingForFrames);
    in.audioReady = true;
    CHECK(mseSeekPhase(in) == MSESeekPhase::Done);
    in.videoReady = false;
    in.secondsSinceReenqueue = mseSeekFrameTimeout + 0.1;
    CHECK(mseSeekPhase(in) == MSESeekPhase::Done);
  }

  TEST_CASE("append logging around seeks") {
    CHECK(mseShouldLogAppend(3, -1, true));
    CHECK_FALSE(mseShouldLogAppend(9, -1, true));
    CHECK(mseShouldLogAppend(40, -1, true));
    CHECK(mseShouldLogAppend(9, 2.0, true));
    CHECK_FALSE(mseShouldLogAppend(9, 31.0, true));
    CHECK(mseShouldLogAppend(9, 31.0, false));
  }

  TEST_CASE("stalls") {
    TimeRangeSet buffered;
    buffered.add(0, 4);
    double inf = std::numeric_limits<double>::infinity();
    CHECK_FALSE(mseShouldStall(buffered, 1, inf, false));
    CHECK(mseShouldStall(buffered, 3.95, inf, false));
    CHECK(mseShouldStall(buffered, 5, inf, false));
    CHECK_FALSE(mseShouldStall(buffered, 3.95, inf, true));
    CHECK_FALSE(mseShouldStall(buffered, 4, 4, false));
    TimeRangeSet none;
    CHECK(mseShouldStall(none, 0, 30, false));
  }
}
