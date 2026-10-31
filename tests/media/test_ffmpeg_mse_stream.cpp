// SPDX-License-Identifier: GPL-3.0-or-later
//
// MSE byte streams delivered in many small appends (YouTube streams a media
// segment in 16-64 KiB pieces): FFmpegAppendParser, built against the host
// libavformat, must produce every sample of real fragmented MP4 and WebM
// exactly once, in order, with the timestamps of a single whole append, and
// must not let partial data pile up. Fixtures: tests/media/data/generate.sh.
#include <doctest/doctest.h>

#include "config.h"
#include "FFmpegAppendParser.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace WebCore;
using FFmpegMedia::ByteStreamFormat;

extern "C" void ps5_log_line(const char*) { }

namespace {

// Partial demuxes end in a cut sample, which libavformat reports.
const int quietLibav = (av_log_set_level(AV_LOG_FATAL), 0);

using Bytes = std::vector<uint8_t>;

Bytes fixture(const char* name)
{
    std::ifstream file(std::string(PROSPEROWEB_MEDIA_TEST_DATA) + "/" + name, std::ios::binary);
    return { std::istreambuf_iterator<char>(file), { } };
}

struct Sample {
    uint64_t track { 0 };
    int64_t dts { 0 };
    int64_t pts { 0 };
    int64_t duration { 0 };
    int size { 0 };
    bool key { false };
    uint32_t checksum { 0 };
    AVRational timeBase { 0, 1 };

    bool operator==(const Sample& other) const
    {
        return track == other.track && dts == other.dts && pts == other.pts && duration == other.duration && size == other.size
            && key == other.key && checksum == other.checksum;
    }
};

struct Recorder final : FFmpegAppendParserClient {
    void appendParserDidParseInitializationSegment(const FFmpegParsedInitializationSegment& segment) final
    {
        ++initializationSegments;
        tracks = segment.tracks.size();
    }

    void appendParserDidParseSample(const FFmpegParsedTrack& track, AVPacket* packet) final
    {
        uint32_t checksum = 2166136261u;
        for (int i = 0; i < packet->size; ++i)
            checksum = (checksum ^ packet->data[i]) * 16777619u;
        samples.push_back({ track.trackID, packet->dts, packet->pts, packet->duration, packet->size, !!(packet->flags & AV_PKT_FLAG_KEY), checksum, track.timeBase });
    }

    // Per track, in delivery order (tracks may interleave differently
    // depending on how the stream was cut).
    std::map<uint64_t, std::vector<Sample>> byTrack() const
    {
        std::map<uint64_t, std::vector<Sample>> result;
        for (auto& sample : samples)
            result[sample.track].push_back(sample);
        return result;
    }

    unsigned initializationSegments { 0 };
    size_t tracks { 0 };
    std::vector<Sample> samples;
};

struct StreamResult {
    bool ok { true };
    std::string error;
    Recorder recorder;
    size_t appends { 0 };
    size_t maxPending { 0 };
    size_t finalPending { 0 };
    size_t partialSamples { 0 };
    unsigned maxFragmentAppends { 0 };
};

// Appends `bytes` in chunks of [minChunk, maxChunk] bytes (seeded).
StreamResult stream(ByteStreamFormat format, const Bytes& bytes, size_t minChunk, size_t maxChunk, unsigned seed, size_t partialStep = FFmpegMedia::SegmentScanner::defaultPartialStep)
{
    StreamResult result;
    FFmpegAppendParser parser(format, partialStep);
    std::mt19937 random(seed);
    std::uniform_int_distribution<size_t> chunk(minChunk, maxChunk);
    for (size_t offset = 0; offset < bytes.size() && result.ok;) {
        size_t size = std::min(chunk(random), bytes.size() - offset);
        result.ok = parser.append(bytes.data() + offset, size, result.recorder);
        if (!result.ok)
            result.error = parser.error();
        offset += size;
        ++result.appends;
        result.maxPending = std::max(result.maxPending, parser.pendingBytes());
        result.partialSamples += parser.lastAppendPartialSamples();
        result.maxFragmentAppends = std::max(result.maxFragmentAppends, parser.lastCompletedFragmentAppends());
    }
    result.finalPending = parser.pendingBytes();
    return result;
}

StreamResult whole(ByteStreamFormat format, const Bytes& bytes)
{
    return stream(format, bytes, bytes.size(), bytes.size(), 0);
}

uint32_t read32(const uint8_t* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void put32(Bytes& out, uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<uint8_t>(value >> shift));
}

Bytes box(const char (&type)[5], size_t payload)
{
    Bytes out;
    put32(out, static_cast<uint32_t>(8 + payload));
    out.insert(out.end(), type, type + 4);
    for (size_t i = 0; i < payload; ++i)
        out.push_back(static_cast<uint8_t>(i * 7));
    return out;
}

// The largest moof + mdat pair of a fragmented MP4.
size_t largestMP4Fragment(const Bytes& mp4)
{
    size_t largest = 0, moof = 0;
    for (size_t offset = 0; offset + 8 <= mp4.size();) {
        uint32_t size = read32(&mp4[offset]);
        uint32_t type = read32(&mp4[offset + 4]);
        if (type == FFmpegMedia::fourCC("moof"))
            moof = size;
        else if (type == FFmpegMedia::fourCC("mdat"))
            largest = std::max<size_t>(largest, moof + size);
        offset += size ? size : mp4.size();
    }
    return largest;
}

// What a DASH/YouTube stream carries between fragments: styp, emsg, prft and
// free before every moof.
Bytes withSegmentBoxes(const Bytes& mp4)
{
    Bytes out;
    for (size_t offset = 0; offset + 8 <= mp4.size();) {
        uint32_t size = read32(&mp4[offset]);
        if (read32(&mp4[offset + 4]) == FFmpegMedia::fourCC("moof")) {
            for (auto& extra : { box("styp", 16), box("emsg", 61), box("prft", 24), box("free", 3000) })
                out.insert(out.end(), extra.begin(), extra.end());
        }
        out.insert(out.end(), mp4.begin() + static_cast<std::ptrdiff_t>(offset), mp4.begin() + static_cast<std::ptrdiff_t>(offset + size));
        offset += size;
    }
    return out;
}

// Rewrites every Cluster of a WebM file with an unknown size (live streams).
Bytes withUnknownSizeClusters(const Bytes& webm, size_t& clusters)
{
    Bytes out;
    clusters = 0;
    size_t offset = 0;
    while (offset < webm.size()) {
        FFmpegMedia::EBMLHeader header;
        if (FFmpegMedia::readEBMLHeader(&webm[offset], webm.size() - offset, header) != FFmpegMedia::ParseStatus::Ok)
            break;
        if (header.id == FFmpegMedia::WebMID::Segment) {
            out.insert(out.end(), webm.begin() + static_cast<std::ptrdiff_t>(offset), webm.begin() + static_cast<std::ptrdiff_t>(offset + header.headerLength));
            offset += header.headerLength;
            continue;
        }
        size_t end = header.unknownSize ? webm.size() : std::min(webm.size(), offset + header.headerLength + static_cast<size_t>(header.size));
        if (header.id == FFmpegMedia::WebMID::Cluster) {
            const uint8_t unknown[] = { 0x1F, 0x43, 0xB6, 0x75, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
            out.insert(out.end(), unknown, unknown + sizeof(unknown));
            out.insert(out.end(), webm.begin() + static_cast<std::ptrdiff_t>(offset + header.headerLength), webm.begin() + static_cast<std::ptrdiff_t>(end));
            ++clusters;
        } else
            out.insert(out.end(), webm.begin() + static_cast<std::ptrdiff_t>(offset), webm.begin() + static_cast<std::ptrdiff_t>(end));
        offset = end;
    }
    // An empty Cues element ends the last cluster (as the next Cluster
    // would).
    const uint8_t cues[] = { 0x1C, 0x53, 0xBB, 0x6B, 0x80 };
    out.insert(out.end(), cues, cues + sizeof(cues));
    return out;
}

// Every run produces the reference samples: same count, and per track the
// same samples (timestamps, durations, flags, payload) in the same order.
bool sameSamples(const StreamResult& run, const StreamResult& reference)
{
    (void)quietLibav;
    return run.recorder.samples.size() == reference.recorder.samples.size() && run.recorder.byTrack() == reference.recorder.byTrack();
}

bool decodeOrderPerTrack(const StreamResult& run)
{
    for (auto& [track, samples] : run.recorder.byTrack()) {
        for (size_t i = 1; i < samples.size(); ++i) {
            if (samples[i].dts <= samples[i - 1].dts)
                return false;
        }
    }
    return true;
}

// Presentation timestamps of a track are distinct and evenly spaced: no
// sample lost or doubled (each gap is at most twice the previous duration;
// muxers round, so exact tiling is not required).
bool evenlySpaced(std::vector<Sample> samples)
{
    std::sort(samples.begin(), samples.end(), [](auto& a, auto& b) { return a.pts < b.pts; });
    for (size_t i = 1; i < samples.size(); ++i) {
        int64_t gap = samples[i].pts - samples[i - 1].pts;
        if (gap <= 0 || samples[i - 1].duration <= 0 || gap > 2 * samples[i - 1].duration)
            return false;
    }
    return !samples.empty();
}

double spanSeconds(const std::vector<Sample>& samples)
{
    int64_t first = samples.front().pts, last = samples.front().pts + samples.front().duration;
    for (auto& sample : samples) {
        first = std::min(first, sample.pts);
        last = std::max(last, sample.pts + sample.duration);
    }
    return static_cast<double>(last - first) * samples.front().timeBase.num / samples.front().timeBase.den;
}

} // namespace

TEST_SUITE("media_mse_stream") {
  TEST_CASE("reference: whole fixtures demux with exact timestamps") {
    auto mp4 = fixture("mse-avc-aac.mp4");
    REQUIRE(mp4.size() > 50000);
    auto reference = whole(ByteStreamFormat::MP4, mp4);
    REQUIRE(reference.ok);
    CHECK(reference.recorder.initializationSegments == 1);
    CHECK(reference.recorder.tracks == 2);
    CHECK(reference.finalPending == 0);
    auto tracks = reference.recorder.byTrack();
    REQUIRE(tracks.size() == 2);
    auto& video = tracks.begin()->second;
    // 4 s of 30 fps H.264, a keyframe every second.
    CHECK(video.size() == 120);
    CHECK(std::count_if(video.begin(), video.end(), [](auto& s) { return s.key; }) == 4);
    CHECK(video.front().key);
    CHECK(video.front().pts == 0);
    CHECK(evenlySpaced(video));
    CHECK(spanSeconds(video) == doctest::Approx(4.0).epsilon(0.02));
    auto& audio = std::next(tracks.begin())->second;
    // AAC: 1024-sample frames at 48 kHz.
    CHECK(audio.size() >= 187);
    CHECK(audio.front().duration * audio.front().timeBase.num * 48000 == 1024 * audio.front().timeBase.den);
    CHECK(evenlySpaced(audio));
    CHECK(spanSeconds(audio) == doctest::Approx(4.0).epsilon(0.05));
    CHECK(decodeOrderPerTrack(reference));

    auto webm = fixture("mse-vp9-opus-live.webm");
    REQUIRE(webm.size() > 50000);
    auto webmReference = whole(ByteStreamFormat::WebM, webm);
    REQUIRE(webmReference.ok);
    CHECK(webmReference.recorder.tracks == 2);
    auto webmTracks = webmReference.recorder.byTrack();
    REQUIRE(webmTracks.size() == 2);
    CHECK(webmTracks.begin()->second.size() == 120);
    CHECK(evenlySpaced(webmTracks.begin()->second));
    CHECK(spanSeconds(webmTracks.begin()->second) == doctest::Approx(4.0).epsilon(0.02));
    CHECK(decodeOrderPerTrack(webmReference));
  }

  TEST_CASE("MP4: random 1-64 KiB, 1-4 KiB and 1-512 byte appends") {
    auto mp4 = fixture("mse-avc-aac.mp4");
    auto reference = whole(ByteStreamFormat::MP4, mp4);
    REQUIRE(reference.ok);
    size_t fragment = largestMP4Fragment(mp4);
    REQUIRE(fragment > 1000);
    struct Range {
        size_t min, max;
    };
    for (auto range : { Range { 1, 65536 }, Range { 1, 4096 }, Range { 1, 512 } }) {
        for (unsigned seed = 1; seed <= 4; ++seed) {
            CAPTURE(range.max);
            CAPTURE(seed);
            auto run = stream(ByteStreamFormat::MP4, mp4, range.min, range.max, seed);
            REQUIRE(run.ok);
            CHECK(sameSamples(run, reference));
            CHECK(run.finalPending == 0);
            // Partial data never exceeds one fragment plus one append.
            CHECK(run.maxPending <= fragment + range.max);
        }
    }
  }

  TEST_CASE("MP4: samples become available while the fragment streams in") {
    auto mp4 = fixture("mse-avc-aac.mp4");
    auto reference = whole(ByteStreamFormat::MP4, mp4);
    REQUIRE(reference.ok);
    for (unsigned seed = 1; seed <= 4; ++seed) {
        CAPTURE(seed);
        // YouTube-like 16-48 KiB pieces scaled to the fixture: 512 B-2 KiB.
        auto run = stream(ByteStreamFormat::MP4, mp4, 512, 2048, seed, 1024);
        REQUIRE(run.ok);
        CHECK(sameSamples(run, reference));
        CHECK(run.partialSamples > reference.recorder.samples.size() / 3);
        CHECK(run.maxFragmentAppends > 2);
        CHECK(run.finalPending == 0);
    }
  }

  TEST_CASE("MP4: styp, sidx, emsg, prft and free boxes between fragments") {
    auto dash = fixture("mse-avc-dash.mp4");
    auto reference = whole(ByteStreamFormat::MP4, dash);
    REQUIRE(reference.ok);
    CHECK(reference.recorder.samples.size() == 120);
    auto boxed = withSegmentBoxes(dash);
    REQUIRE(boxed.size() > dash.size());
    auto boxedWhole = whole(ByteStreamFormat::MP4, boxed);
    REQUIRE(boxedWhole.ok);
    CHECK(sameSamples(boxedWhole, reference));
    for (unsigned seed = 1; seed <= 4; ++seed) {
        CAPTURE(seed);
        auto run = stream(ByteStreamFormat::MP4, boxed, 1, 3000, seed, 2048);
        REQUIRE(run.ok);
        CHECK(sameSamples(run, reference));
        CHECK(run.finalPending == 0);
    }
  }

  TEST_CASE("WebM: known-size clusters in random appends") {
    auto webm = fixture("mse-vp9-opus-live.webm");
    auto reference = whole(ByteStreamFormat::WebM, webm);
    REQUIRE(reference.ok);
    for (unsigned seed = 1; seed <= 4; ++seed) {
        CAPTURE(seed);
        auto big = stream(ByteStreamFormat::WebM, webm, 1, 65536, seed);
        REQUIRE(big.ok);
        CHECK(sameSamples(big, reference));
        auto small = stream(ByteStreamFormat::WebM, webm, 1, 1500, seed, 1024);
        REQUIRE(small.ok);
        CHECK(sameSamples(small, reference));
        CHECK(small.partialSamples > 0);
        CHECK(small.finalPending == 0);
    }
  }

  TEST_CASE("WebM: live stream with unknown-size clusters") {
    size_t clusters = 0;
    auto live = withUnknownSizeClusters(fixture("mse-vp9-opus-live.webm"), clusters);
    REQUIRE(clusters >= 4);
    auto reference = whole(ByteStreamFormat::WebM, fixture("mse-vp9-opus-live.webm"));
    REQUIRE(reference.ok);
    auto liveWhole = whole(ByteStreamFormat::WebM, live);
    REQUIRE(liveWhole.ok);
    CHECK(sameSamples(liveWhole, reference));
    for (unsigned seed = 1; seed <= 4; ++seed) {
        CAPTURE(seed);
        auto run = stream(ByteStreamFormat::WebM, live, 1, 2048, seed, 1024);
        REQUIRE(run.ok);
        CHECK(sameSamples(run, reference));
        CHECK(run.partialSamples > 0);
        CHECK(run.finalPending == 0);
    }
  }
}
