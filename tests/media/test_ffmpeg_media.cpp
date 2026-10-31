// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <limits>
#include <optional>

#include "FFmpegAVClock.h"
#include "FFmpegPlaybackPolicy.h"
#include "FFmpegRingBuffer.h"
#include "FFmpegSupportTable.h"
#include "FFmpegTimeRanges.h"

using namespace WebCore::FFmpegMedia;

namespace {

Support check(std::string_view mime, std::string_view codecs = { }, DecoderAvailability decoders = { })
{
    return evaluate(mime, splitCodecs(codecs), decoders);
}

} // namespace

TEST_SUITE("media_support") {
  TEST_CASE("containers") {
    CHECK(containerForMIMEType("video/mp4") == Container::MP4);
    CHECK(containerForMIMEType("VIDEO/WebM") == Container::WebM);
    CHECK(containerForMIMEType(" application/x-mpegURL ") == Container::HLS);
    CHECK(containerForMIMEType("application/vnd.apple.mpegurl") == Container::HLS);
    CHECK(containerForMIMEType("audio/mpegurl") == Container::HLS);
    CHECK(containerForMIMEType("audio/mpeg") == Container::MPEG);
    CHECK(containerForMIMEType("video/x-flv") == Container::Unknown);
  }

  TEST_CASE("no codecs parameter is maybe, unknown container is no") {
    CHECK(check("video/mp4") == Support::MaybeSupported);
    CHECK(check("audio/webm") == Support::MaybeSupported);
    CHECK(check("video/avi", "avc1.42E01E") == Support::NotSupported);
    CHECK(check("") == Support::NotSupported);
  }

  TEST_CASE("supported codec combinations are probably") {
    CHECK(check("video/mp4", "avc1.64001F, mp4a.40.2") == Support::IsSupported);
    CHECK(check("video/mp4", "hvc1.1.6.L93.B0") == Support::IsSupported);
    CHECK(check("video/mp4", "hev1.1.6.L93.B0,mp4a.40.5") == Support::IsSupported);
    CHECK(check("video/webm", "vp8, vorbis") == Support::IsSupported);
    CHECK(check("video/webm", "vp09.00.10.08, opus") == Support::IsSupported);
    CHECK(check("audio/webm", "opus") == Support::IsSupported);
    CHECK(check("audio/mp4", "mp4a.40.2") == Support::IsSupported);
    CHECK(check("audio/mpeg", "mp3") == Support::IsSupported);
    CHECK(check("application/vnd.apple.mpegurl", "avc1.4d401f,mp4a.40.2") == Support::IsSupported);
    CHECK(check("audio/ogg", "VORBIS") == Support::IsSupported);
  }

  TEST_CASE("av1 depends on a software decoder") {
    CHECK(check("video/mp4", "av01.0.05M.08") == Support::NotSupported);
    DecoderAvailability withDav1d;
    withDav1d.av1 = true;
    CHECK(check("video/mp4", "av01.0.05M.08", withDav1d) == Support::IsSupported);
    CHECK(check("video/webm", "av01.0.05M.08,opus", withDav1d) == Support::IsSupported);
  }

  TEST_CASE("codecs impossible in the container are rejected") {
    CHECK(check("video/webm", "avc1.42E01E") == Support::NotSupported);
    CHECK(check("audio/mpeg", "opus") == Support::NotSupported);
    CHECK(check("video/mp4", "vorbis") == Support::NotSupported);
  }

  TEST_CASE("unknown codec strings are maybe") {
    CHECK(check("video/mp4", "avc1.64001F, xyz1") == Support::MaybeSupported);
    CHECK(check("video/mp4", "dvh1.05.06") == Support::MaybeSupported);
    // An unsupported codec wins over an unknown one.
    CHECK(check("video/webm", "xyz1, avc1.42E01E") == Support::NotSupported);
  }

  TEST_CASE("codec families and splitting") {
    CHECK(codecFamily("mp4a.40.34") == CodecFamily::MP3);
    CHECK(codecFamily("mp4a.69") == CodecFamily::MP3);
    CHECK(codecFamily("mp4a.40.29") == CodecFamily::AAC);
    CHECK(codecFamily("ec-3") == CodecFamily::EAC3);
    CHECK(codecFamily("avc3.640028") == CodecFamily::H264);
    auto parts = splitCodecs(" avc1.64001F ,, MP4A.40.2 ");
    REQUIRE(parts.size() == 2);
    CHECK(parts[0] == "avc1.64001f");
    CHECK(parts[1] == "mp4a.40.2");
    CHECK(splitCodecs("").empty());
  }
}

TEST_SUITE("media_ranges") {
  TEST_CASE("disjoint ranges stay sorted") {
    TimeRangeSet set;
    set.add(10, 20);
    set.add(0, 5);
    set.add(30, 40);
    REQUIRE(set.size() == 3);
    CHECK(set[0].start == 0);
    CHECK(set[1].start == 10);
    CHECK(set[2].end == 40);
    CHECK(set.totalDuration() == doctest::Approx(25));
  }

  TEST_CASE("overlapping and adjacent ranges merge") {
    TimeRangeSet set;
    set.add(0, 5);
    set.add(4, 8);
    set.add(8.01, 9); // within the merge tolerance
    REQUIRE(set.size() == 1);
    CHECK(set[0].start == 0);
    CHECK(set[0].end == 9);

    set.add(20, 30);
    set.add(15, 25);
    set.add(-1, 16); // bridges everything
    REQUIRE(set.size() == 1);
    CHECK(set[0].start == -1);
    CHECK(set[0].end == 30);
  }

  TEST_CASE("invalid ranges are ignored") {
    TimeRangeSet set;
    set.add(5, 5);
    set.add(6, 2);
    set.add(0, std::numeric_limits<double>::infinity());
    set.add(std::numeric_limits<double>::quiet_NaN(), 1);
    CHECK(set.isEmpty());
  }

  TEST_CASE("buffered ahead and contains") {
    TimeRangeSet set;
    set.add(0, 10);
    set.add(20, 25);
    CHECK(set.contains(0));
    CHECK(set.contains(9.99));
    CHECK_FALSE(set.contains(10));
    CHECK_FALSE(set.contains(15));
    CHECK(set.bufferedAhead(4) == doctest::Approx(6));
    CHECK(set.bufferedAhead(22) == doctest::Approx(3));
    CHECK(set.bufferedAhead(12) == 0);
    set.clear();
    CHECK(set.bufferedAhead(1) == 0);
  }
}

TEST_SUITE("media_clock") {
  TEST_CASE("audio clock math") {
    CHECK(audioClockTime(10, 48000, 48000, 0) == doctest::Approx(11));
    CHECK(audioClockTime(10, 48000, 48000, 0.25) == doctest::Approx(10.75));
    // Never before the base, even with latency larger than what was played.
    CHECK(audioClockTime(10, 100, 48000, 0.5) == doctest::Approx(10));
    CHECK(audioClockTime(3, 1000, 0, 0) == doctest::Approx(3));
  }

  TEST_CASE("extrapolated clock runs, pauses and changes rate") {
    AVClock clock;
    clock.reset(5, 100);
    CHECK(clock.time(101) == doctest::Approx(5)); // not running
    clock.setRunning(true, 101);
    CHECK(clock.time(102) == doctest::Approx(6));
    clock.setRunning(false, 102.5);
    CHECK(clock.time(110) == doctest::Approx(6.5));
    clock.setRunning(true, 110);
    clock.setRate(2, 110);
    CHECK(clock.time(111) == doctest::Approx(8.5));
    CHECK_FALSE(clock.audioCanBeMaster());
  }

  TEST_CASE("stall holds the extrapolated clock") {
    AVClock clock;
    clock.reset(0, 0);
    clock.setRunning(true, 0);
    clock.setStalled(true, 1);
    CHECK(clock.time(5) == doctest::Approx(1));
    clock.setStalled(false, 5);
    CHECK(clock.time(6) == doctest::Approx(2));
  }

  TEST_CASE("audio master and fallback when audio ends") {
    AVClock clock;
    clock.reset(0, 0);
    clock.setRunning(true, 0);
    CHECK(clock.time(1, 0.9) == doctest::Approx(0.9));
    // Audio slightly behind its previous value: the clock never goes back.
    CHECK(clock.time(1.01, 0.85) == doctest::Approx(0.9));
    // Audio gone: continue from the last audio anchor at wall-clock speed.
    CHECK(clock.time(2.01) == doctest::Approx(1.9));
    // A seek resets monotonicity.
    clock.reset(0.2, 3);
    CHECK(clock.time(3) == doctest::Approx(0.2));
  }

  TEST_CASE("frame decisions") {
    CHECK(decideFrame(1.0, std::nullopt, 0.5) == FrameDecision::Wait);
    CHECK(decideFrame(1.0, 1.04, 1.0) == FrameDecision::Show);
    CHECK(decideFrame(1.0, 1.04, 1.05) == FrameDecision::Drop);
    CHECK(decideFrame(1.0, std::nullopt, 3.0) == FrameDecision::Show);
  }
}

TEST_SUITE("media_ring") {
  TEST_CASE("write, read and wrap around") {
    RingBuffer<int> ring(4);
    int in[] = { 1, 2, 3, 4, 5 };
    CHECK(ring.write(in, 5) == 4);
    CHECK(ring.freeSpace() == 0);
    int out[4] = { };
    CHECK(ring.read(out, 3) == 3);
    CHECK(out[0] == 1);
    CHECK(out[2] == 3);
    CHECK(ring.write(in + 4, 1) == 1); // wraps
    CHECK(ring.write(in, 2) == 2);
    CHECK(ring.size() == 4);
    CHECK(ring.read(out, 4) == 4);
    CHECK(out[0] == 4);
    CHECK(out[1] == 5);
    CHECK(out[2] == 1);
    CHECK(out[3] == 2);
    CHECK(ring.isEmpty());
  }

  TEST_CASE("fill, skip and clear") {
    RingBuffer<float> ring(8);
    CHECK(ring.fill(0.5f, 10) == 8);
    CHECK(ring.skip(3) == 3);
    float out[8] = { };
    CHECK(ring.read(out, 8) == 5);
    CHECK(out[4] == 0.5f);
    ring.fill(1, 2);
    ring.clear();
    CHECK(ring.isEmpty());
    CHECK(ring.read(out, 1) == 0);
    RingBuffer<float> degenerate(0);
    CHECK(degenerate.capacity() == 1);
  }
}

TEST_SUITE("media_policy") {
  TEST_CASE("network read-ahead is capped with and without Range or a size") {
    const uint64_t mib = 1u << 20;
    const uint64_t high = networkHighWaterBytes;
    const uint64_t noRange = networkNoRangeHighWaterBytes;
    CHECK(high == 8 * mib);
    CHECK(noRange > high);
    // Range support, known size: the high water mark.
    CHECK_FALSE(networkShouldSuspend(high, 100 * mib, 1000 * mib, true));
    CHECK(networkShouldSuspend(high + 1, 100 * mib, 1000 * mib, true));
    // Unknown size (chunked) is capped the same way.
    CHECK(networkShouldSuspend(high + mib, 100 * mib, -1, true));
    // No Range support: a higher cap, but still a cap.
    CHECK_FALSE(networkShouldSuspend(high + mib, 100 * mib, -1, false));
    CHECK(networkShouldSuspend(noRange + 1, 100 * mib, -1, false));
    CHECK(networkShouldSuspend(noRange + 1, 100 * mib, 1000 * mib, false));
    // The whole resource is here: never suspend.
    CHECK_FALSE(networkShouldSuspend(100 * mib, 1000 * mib, 1000 * mib, true));
    CHECK(networkShouldResume(networkLowWaterBytes - 1));
    CHECK_FALSE(networkShouldResume(networkLowWaterBytes));
  }

  TEST_CASE("video frame rows leave room for swscale's 8-pixel stores") {
    for (int width : { 2, 426, 480, 640, 854, 1278, 1280, 1366, 1920 }) {
      const size_t stride = videoFrameStride(width);
      const size_t rounded = static_cast<size_t>((width + 7) & ~7) * 4;
      CHECK(stride % 64 == 0);
      CHECK(stride >= rounded);
      // The last row's overrun stays inside the buffer.
      CHECK(videoFrameBytes(width, 270) >= stride * 269 + rounded);
    }
    CHECK(videoFrameStride(0) == 0);
    CHECK(videoFrameBytes(1920, 0) == 0);
  }

  TEST_CASE("demuxer read-ahead") {
    QueueLevel full { true, 400, 20 };
    QueueLevel low { true, 10, 1 };
    QueueLevel off { };
    CHECK(demuxerShouldWait(0, full, full));
    CHECK_FALSE(demuxerShouldWait(0, full, low));
    CHECK(demuxerShouldWait(0, full, off));
    CHECK(demuxerShouldWait(maxQueuedBytes, low, low));
    CHECK_FALSE(demuxerShouldWait(0, off, off));
    // Many seconds in few packets (sparse stream) is not "enough".
    CHECK_FALSE(demuxerShouldWait(0, QueueLevel { true, 3, 60 }, full));
  }

  TEST_CASE("ready state follows the HTML thresholds") {
    ReadyInputs in;
    CHECK(computeReadyLevel(in) == ReadyLevel::HaveNothing);
    in.metadataLoaded = true;
    CHECK(computeReadyLevel(in) == ReadyLevel::HaveMetadata);
    in.currentDataAvailable = true;
    CHECK(computeReadyLevel(in) == ReadyLevel::HaveCurrentData);
    in.bufferedAheadSeconds = 1;
    CHECK(computeReadyLevel(in) == ReadyLevel::HaveFutureData);
    in.bufferedAheadSeconds = 5;
    CHECK(computeReadyLevel(in) == ReadyLevel::HaveEnoughData);
    in.seeking = true;
    CHECK(computeReadyLevel(in) == ReadyLevel::HaveMetadata);
    in.seeking = false;
    in.bufferedAheadSeconds = 0;
    in.endOfStream = true;
    CHECK(computeReadyLevel(in) == ReadyLevel::HaveEnoughData);
  }

  TEST_CASE("network state") {
    NetworkInputs in;
    CHECK(computeNetworkLevel(in) == NetworkLevel::Idle);
    in.started = true;
    CHECK(computeNetworkLevel(in) == NetworkLevel::Loading);
    in.readerWaiting = true;
    CHECK(computeNetworkLevel(in) == NetworkLevel::Loading); // no metadata yet
    in.metadataLoaded = true;
    CHECK(computeNetworkLevel(in) == NetworkLevel::Idle);
    in.endOfStream = true;
    CHECK(computeNetworkLevel(in) == NetworkLevel::Loaded);
    in.failure = LoadFailure::Network;
    CHECK(computeNetworkLevel(in) == NetworkLevel::NetworkError);
    in.metadataLoaded = false;
    CHECK(computeNetworkLevel(in) == NetworkLevel::FormatError);
    in.failure = LoadFailure::Decode;
    CHECK(computeNetworkLevel(in) == NetworkLevel::DecodeError);
  }
}
