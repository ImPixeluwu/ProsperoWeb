// SPDX-License-Identifier: GPL-3.0-or-later
//
// Remote (UI-side hardware) video decoding: AVCC/hvcC to Annex-B with the
// parameter sets on keyframes, the pts and picture-slot bookkeeping and the
// show policy, NV12 -> ARGB against a floating-point reference, the
// remote-decoding wire messages, and a loopback run of the WebProcess client
// (RemoteVideoStream) against the overlay server with a fake decoder.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <doctest/doctest.h>
#include <unistd.h>

#include "FFmpegVideoOverlayClient.h"
#include "FFmpegVideoOverlayWire.h"
#include "hwdec_bitstream.h"
#include "hwdec_policy.h"
#include "hwdec_stream.h"
#include "nv12_convert.h"
#include "video_server.h"

namespace wire = WebCore::FFmpegMedia::VideoWire;
using namespace prosperoweb;
using video::VideoCodec;

namespace {

using Bytes = std::vector<std::uint8_t>;

// SPS/PPS/IDR/non-IDR NAL units (contents beyond the header are arbitrary).
const Bytes kSps = {0x67, 0x64, 0x00, 0x28, 0xAC, 0xD9};
const Bytes kPps = {0x68, 0xEB, 0xE3, 0xCB};
const Bytes kIdr = {0x65, 0x88, 0x84, 0x00, 0x33};
const Bytes kSlice = {0x41, 0x9A, 0x02, 0x00};
const Bytes kAud = {0x09, 0xF0};

Bytes avcc_record(int length_size = 4) {
  Bytes r = {1, 0x64, 0x00, 0x28, static_cast<std::uint8_t>(0xFC | (length_size - 1)), 0xE1};
  r.push_back(0);
  r.push_back(static_cast<std::uint8_t>(kSps.size()));
  r.insert(r.end(), kSps.begin(), kSps.end());
  r.push_back(1);
  r.push_back(0);
  r.push_back(static_cast<std::uint8_t>(kPps.size()));
  r.insert(r.end(), kPps.begin(), kPps.end());
  // High profile extension: 4:2:0, 8-bit.
  r.push_back(0xFC | 1);
  r.push_back(0xF8 | 0);
  r.push_back(0xF8 | 0);
  r.push_back(0);
  return r;
}

Bytes length_prefixed(const std::vector<Bytes>& nals, int length_size = 4) {
  Bytes out;
  for (const Bytes& n : nals) {
    for (int i = length_size - 1; i >= 0; --i) {
      out.push_back(static_cast<std::uint8_t>(n.size() >> (8 * i)));
    }
    out.insert(out.end(), n.begin(), n.end());
  }
  return out;
}

Bytes annexb(const std::vector<Bytes>& nals, bool short_codes = false) {
  Bytes out;
  for (const Bytes& n : nals) {
    if (!short_codes) {
      out.push_back(0);
    }
    out.insert(out.end(), {0, 0, 1});
    out.insert(out.end(), n.begin(), n.end());
  }
  return out;
}

// hvcC with VPS/SPS/PPS arrays (HEVC NAL headers: type << 1).
const Bytes kHevcVps = {0x40, 0x01, 0x0C, 0x01};
const Bytes kHevcSps = {0x42, 0x01, 0x01, 0x01};
const Bytes kHevcPps = {0x44, 0x01, 0xC1, 0x72};
const Bytes kHevcIdr = {0x26, 0x01, 0xAF, 0x08};  // IDR_W_RADL (19)

Bytes hvcc_record(int profile = 1, int bit_depth = 8) {
  Bytes r(23, 0);
  r[0] = 1;
  r[1] = static_cast<std::uint8_t>(profile);
  r[12] = 120;  // level 4
  r[16] = 0xFC | 1;
  r[17] = static_cast<std::uint8_t>(0xF8 | (bit_depth - 8));
  r[18] = static_cast<std::uint8_t>(0xF8 | (bit_depth - 8));
  r[21] = 0x0F | 3;  // length size 4
  r[22] = 3;
  for (const Bytes* nal : {&kHevcVps, &kHevcSps, &kHevcPps}) {
    r.push_back(static_cast<std::uint8_t>(0x80 | ((*nal)[0] >> 1)));
    r.push_back(0);
    r.push_back(1);
    r.push_back(0);
    r.push_back(static_cast<std::uint8_t>(nal->size()));
    r.insert(r.end(), nal->begin(), nal->end());
  }
  return r;
}

template <typename F>
bool wait_until(F&& condition, int timeout_ms = 3000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return condition();
}

int channel(std::uint32_t pixel, int shift) { return static_cast<int>((pixel >> shift) & 0xFF); }

}  // namespace

TEST_SUITE("hwdec_bitstream") {
  TEST_CASE("avcC: profile, level, length size and parameter sets") {
    const Bytes record = avcc_record();
    std::string error;
    auto config = video::parse_stream_config(VideoCodec::H264, record.data(), record.size(), error);
    REQUIRE(config);
    CHECK(config->profile == 100);
    CHECK(config->level == 40);
    CHECK(config->nal_length_size == 4);
    CHECK(config->chroma_format == 1);
    CHECK(config->bit_depth_luma == 8);
    CHECK(config->parameter_sets == annexb({kSps, kPps}));
    std::string reason;
    CHECK(video::hardware_profile_supported(*config, true, reason));

    Bytes truncated(record.begin(), record.begin() + 9);
    CHECK_FALSE(video::parse_stream_config(VideoCodec::H264, truncated.data(), truncated.size(),
                                           error));
    const Bytes junk = {7, 1, 2, 3};
    CHECK_FALSE(video::parse_stream_config(VideoCodec::H264, junk.data(), junk.size(), error));
  }

  TEST_CASE("AVCC keyframes get SPS/PPS in front, other frames are only re-framed") {
    const Bytes record = avcc_record();
    std::string error;
    auto config = video::parse_stream_config(VideoCodec::H264, record.data(), record.size(), error);
    REQUIRE(config);
    Bytes out;
    const Bytes key = length_prefixed({kIdr});
    REQUIRE(video::to_annexb(*config, key.data(), key.size(), true, out));
    CHECK(out == annexb({kSps, kPps, kIdr}));
    CHECK(video::annexb_nal_types(VideoCodec::H264, out.data(), out.size()) ==
          std::vector<int>{7, 8, 5});

    const Bytes delta = length_prefixed({kSlice});
    REQUIRE(video::to_annexb(*config, delta.data(), delta.size(), false, out));
    CHECK(out == annexb({kSlice}));

    // An access unit delimiter stays first.
    const Bytes with_aud = length_prefixed({kAud, kIdr});
    REQUIRE(video::to_annexb(*config, with_aud.data(), with_aud.size(), true, out));
    CHECK(video::annexb_nal_types(VideoCodec::H264, out.data(), out.size()) ==
          std::vector<int>{9, 7, 8, 5});

    // In-band parameter sets are not doubled.
    const Bytes inband = length_prefixed({kSps, kPps, kIdr});
    REQUIRE(video::to_annexb(*config, inband.data(), inband.size(), true, out));
    CHECK(video::annexb_nal_types(VideoCodec::H264, out.data(), out.size()) ==
          std::vector<int>{7, 8, 5});

    // A length running past the end is refused.
    Bytes broken = length_prefixed({kSlice});
    broken[3] = 40;
    CHECK_FALSE(video::to_annexb(*config, broken.data(), broken.size(), false, out));
  }

  TEST_CASE("2-byte length prefixes") {
    const Bytes record = avcc_record(2);
    std::string error;
    auto config = video::parse_stream_config(VideoCodec::H264, record.data(), record.size(), error);
    REQUIRE(config);
    CHECK(config->nal_length_size == 2);
    const Bytes au = length_prefixed({kSlice, kSlice}, 2);
    Bytes out;
    REQUIRE(video::to_annexb(*config, au.data(), au.size(), false, out));
    CHECK(out == annexb({kSlice, kSlice}));
  }

  TEST_CASE("Annex-B extradata and packets (3- and 4-byte start codes)") {
    const Bytes extradata = annexb({kSps, kPps}, true);
    std::string error;
    auto config =
        video::parse_stream_config(VideoCodec::H264, extradata.data(), extradata.size(), error);
    REQUIRE(config);
    CHECK(config->nal_length_size == 0);
    CHECK(config->profile == 0x64);
    CHECK(config->parameter_sets == annexb({kSps, kPps}));
    const Bytes key = annexb({kIdr}, true);
    Bytes out;
    REQUIRE(video::to_annexb(*config, key.data(), key.size(), true, out));
    CHECK(out == annexb({kSps, kPps, kIdr}));

    // No extradata at all: Annex-B with in-band parameter sets, passed on.
    auto bare = video::parse_stream_config(VideoCodec::H264, nullptr, 0, error);
    REQUIRE(bare);
    const Bytes inband = annexb({kSps, kPps, kIdr});
    REQUIRE(video::to_annexb(*bare, inband.data(), inband.size(), true, out));
    CHECK(out == inband);
  }

  TEST_CASE("hvcC: arrays, profile and bit depth; HEVC keyframes get VPS/SPS/PPS") {
    const Bytes record = hvcc_record();
    std::string error;
    auto config = video::parse_stream_config(VideoCodec::Hevc, record.data(), record.size(), error);
    REQUIRE(config);
    CHECK(config->profile == 1);
    CHECK(config->level == 120);
    CHECK(config->nal_length_size == 4);
    CHECK(config->parameter_sets == annexb({kHevcVps, kHevcSps, kHevcPps}));
    Bytes out;
    const Bytes key = length_prefixed({kHevcIdr});
    REQUIRE(video::to_annexb(*config, key.data(), key.size(), true, out));
    CHECK(video::annexb_nal_types(VideoCodec::Hevc, out.data(), out.size()) ==
          std::vector<int>{32, 33, 34, 19});
    std::string reason;
    CHECK(video::hardware_profile_supported(*config, true, reason));
    CHECK_FALSE(video::hardware_profile_supported(*config, false, reason));

    const Bytes main10 = hvcc_record(2, 10);
    auto config10 = video::parse_stream_config(VideoCodec::Hevc, main10.data(), main10.size(), error);
    REQUIRE(config10);
    CHECK(config10->bit_depth_luma == 10);
    CHECK_FALSE(video::hardware_profile_supported(*config10, true, reason));
    CHECK(reason.find("10-bit") != std::string::npos);
  }

  TEST_CASE("H.264 profiles the decoder takes") {
    video::StreamConfig config;
    std::string reason;
    for (int profile : {66, 77, 100}) {
      config.profile = profile;
      CHECK(video::hardware_profile_supported(config, false, reason));
    }
    config.profile = 110;  // High 10
    CHECK_FALSE(video::hardware_profile_supported(config, false, reason));
    config.profile = 100;
    config.chroma_format = 2;  // 4:2:2
    CHECK_FALSE(video::hardware_profile_supported(config, false, reason));
  }
}

TEST_SUITE("hwdec_policy") {
  TEST_CASE("pictures take the smallest pending pts, or the one the decoder reports") {
    video::PtsQueue q;
    for (std::int64_t pts : {0, 3000, 1000, 2000}) {  // decode order of an IPBB GOP
      q.add(pts);
    }
    CHECK(q.take() == 0);
    CHECK(q.take() == 1000);
    // The decoder says 3000: 2000 is stale (its packet made no picture).
    CHECK(q.take(3000) == 3000);
    CHECK(q.size() == 0);
    CHECK(q.take() == std::nullopt);
    for (int i = 0; i < 100; ++i) {
      q.add(i);
    }
    CHECK(q.size() == video::PtsQueue::kMaxPending);
    q.clear();
    CHECK(q.size() == 0);
  }

  TEST_CASE("slots: decoder-owned and held slots are not free") {
    video::FrameSlots slots(3);
    CHECK(slots.find_free() == 0);
    slots.set_decoder(0, true);
    CHECK(slots.find_free() == 1);
    slots.hold(0, 100);  // came out of the decoder in slot 0
    CHECK(slots.decoder_count() == 0);
    CHECK(slots.held_count() == 1);
    CHECK(slots.find_free() == 1);
    slots.set_decoder(1, true);
    slots.set_decoder(2, true);
    CHECK(slots.find_free() == -1);
    slots.reset_decoder();
    CHECK(slots.find_free() == 1);
  }

  TEST_CASE("show picks the newest picture at or before pts and releases older ones") {
    video::FrameSlots slots(4);
    slots.hold(0, 100);
    slots.hold(1, 200);
    slots.hold(2, 300);
    auto r = slots.show(250);
    CHECK(r.slot == 1);
    CHECK_FALSE(r.exact);
    CHECK(r.changed);
    CHECK(r.released == 1);  // 100 was skipped
    CHECK(slots.current() == 1);
    CHECK(slots.is_free(0));
    CHECK_FALSE(slots.is_free(1));  // on screen
    r = slots.show(300);
    CHECK(r.slot == 2);
    CHECK(r.exact);
    CHECK(slots.is_free(1));  // the previous picture
    // Nothing newer: the current one stays.
    r = slots.show(350);
    CHECK(r.slot == 2);
    CHECK_FALSE(r.changed);
    // Before everything held: nothing.
    slots.hold(3, 400);
    CHECK(slots.show(50).slot == -1);
  }

  TEST_CASE("discard, flush and eviction release held pictures; pins keep the converted one") {
    video::FrameSlots slots(4);
    slots.hold(0, 10);
    slots.hold(1, 20);
    slots.hold(2, 30);
    CHECK(slots.discard(20));
    CHECK_FALSE(slots.discard(20));
    CHECK(slots.is_free(1));
    CHECK(slots.evict_oldest());
    CHECK(slots.is_free(0));
    CHECK(slots.show(30).slot == 2);
    slots.pin(2);
    slots.hold(3, 40);
    CHECK(slots.show(40).slot == 3);
    CHECK_FALSE(slots.is_free(2));  // the converter still reads it
    slots.unpin(2);
    CHECK(slots.is_free(2));
    slots.hold(0, 50);
    CHECK(slots.release_held() == 1);
    CHECK(slots.current() == 3);  // a seek keeps the picture on screen
    CHECK_FALSE(slots.evict_oldest());
  }
}

TEST_SUITE("hwdec_convert") {
  struct Picture {
    int w, h;
    Bytes y, uv;
    video::Nv12View view() const {
      return {y.data(), uv.data(), w + 8, w + 8, w, h};
    }
  };

  Picture random_picture(int w, int h, std::uint32_t seed) {
    std::mt19937 rng(seed);
    Picture p{w, h, Bytes(static_cast<std::size_t>(w + 8) * h),
              Bytes(static_cast<std::size_t>(w + 8) * (h / 2))};
    for (auto& v : p.y) {
      v = static_cast<std::uint8_t>(rng());
    }
    for (auto& v : p.uv) {
      v = static_cast<std::uint8_t>(rng());
    }
    return p;
  }

  int max_diff(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
    int worst = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
      for (int shift : {0, 8, 16, 24}) {
        worst = std::max(worst, std::abs(channel(a[i], shift) - channel(b[i], shift)));
      }
    }
    return worst;
  }

  TEST_CASE("matches the reference for every matrix and range, full size and halved") {
    const Picture p = random_picture(70, 38, 5);
    std::vector<std::int32_t> scratch;
    for (auto matrix : {video::YuvMatrix::Bt601, video::YuvMatrix::Bt709,
                        video::YuvMatrix::Bt2020}) {
      for (bool full : {false, true}) {
        for (int step : {1, 2}) {
          const video::YuvColor color{matrix, full};
          const int w = p.w / step;
          const int h = p.h / step;
          std::vector<std::uint32_t> got(static_cast<std::size_t>(w) * h);
          std::vector<std::uint32_t> want(got.size());
          video::nv12_to_argb(p.view(), color, step, got.data(), w, scratch);
          video::nv12_to_argb_reference(p.view(), color, step, want.data(), w);
          CHECK(max_diff(got, want) <= 1);
        }
      }
    }
  }

  TEST_CASE("the AVX2 rows and the portable loops agree exactly") {
    const Picture p = random_picture(1928, 6, 11);  // 8-pixel blocks plus a tail
    std::vector<std::int32_t> scratch;
    std::vector<std::uint32_t> simd(static_cast<std::size_t>(p.w) * p.h);
    std::vector<std::uint32_t> portable(simd.size());
    const video::YuvColor color{video::YuvMatrix::Bt709, false};
    video::nv12_to_argb(p.view(), color, 1, simd.data(), p.w, scratch);
    video::set_nv12_simd(false);
    video::nv12_to_argb(p.view(), color, 1, portable.data(), p.w, scratch);
    video::set_nv12_simd(true);
    CHECK(simd == portable);
  }

  TEST_CASE("limited-range black, white and grey") {
    Picture p{4, 2, Bytes(12 * 2), Bytes(12 * 1, 128)};
    std::fill(p.y.begin(), p.y.begin() + 12, 16);
    std::fill(p.y.begin() + 12, p.y.end(), 235);
    std::vector<std::uint32_t> out(8);
    std::vector<std::int32_t> scratch;
    video::nv12_to_argb(p.view(), {video::YuvMatrix::Bt709, false}, 1, out.data(), 4, scratch);
    CHECK(out[0] == 0xFF000000u);
    CHECK(out[7] == 0xFFFFFFFFu);
  }
}

TEST_SUITE("hwdec_wire") {
  TEST_CASE("stream-open, packet and control messages round-trip") {
    wire::StreamOpen open;
    open.stream = 9;
    open.codec = wire::Codec::Hevc;
    open.matrix = wire::Matrix::Bt709;
    open.fullRange = true;
    open.profile = 1;
    open.level = 150;
    open.width = 3840;
    open.height = 2160;
    open.timeBaseNum = 1;
    open.timeBaseDen = 90000;
    open.extradata = hvcc_record();
    Bytes out;
    wire::appendStreamOpen(out, open);
    auto decoded = wire::decodeStreamOpen(out.data() + wire::headerSize, out.size() - wire::headerSize);
    REQUIRE(decoded);
    CHECK(decoded->codec == wire::Codec::Hevc);
    CHECK(decoded->matrix == wire::Matrix::Bt709);
    CHECK(decoded->fullRange);
    CHECK(decoded->level == 150);
    CHECK(decoded->timeBaseDen == 90000);
    CHECK(decoded->extradata == open.extradata);

    out.clear();
    const Bytes data = {1, 2, 3, 4, 5};
    wire::appendPacket(out, {9, 3, -2000, -4000, true}, data.data(), data.size());
    const std::uint8_t* got = nullptr;
    std::size_t got_bytes = 0;
    auto packet = wire::decodePacket(out.data() + wire::headerSize, out.size() - wire::headerSize,
                                     &got, &got_bytes);
    REQUIRE(packet);
    CHECK(packet->pts == -2000);
    CHECK(packet->dts == -4000);
    CHECK(packet->keyframe);
    CHECK(Bytes(got, got + got_bytes) == data);

    out.clear();
    wire::appendStreamControl(out, wire::Type::Show, {9, 3, 123456789012LL});
    auto show = wire::decodeStreamControl(wire::Type::Show, out.data() + wire::headerSize,
                                          out.size() - wire::headerSize);
    REQUIRE(show);
    CHECK(show->pts == 123456789012LL);
    CHECK_FALSE(wire::decodeStreamControl(wire::Type::Flush, out.data() + wire::headerSize,
                                          out.size() - wire::headerSize));

    out.clear();
    wire::appendDecoded(out, {9, 3, 77, wire::Decoded::HasFrame, 1920, 1080, 4242});
    auto notice = wire::decodeDecoded(out.data() + wire::headerSize, out.size() - wire::headerSize);
    REQUIRE(notice);
    CHECK(notice->consumed == 77);
    CHECK(notice->hasFrame());
    CHECK_FALSE(notice->drained());

    out.clear();
    wire::appendStreamFailure(out, wire::Type::StreamReject, {9, "unsupported"});
    auto failure = wire::decodeStreamFailure(out.data() + wire::headerSize, out.size() - wire::headerSize);
    REQUIRE(failure);
    CHECK(failure->reason == "unsupported");
  }
}

namespace {

// A decoder with one picture of delay: each access unit's picture comes
// out of the next call, written into the slot it was given (and kept
// meanwhile). The picture's luma is 16 + pts % 200, chroma neutral.
struct FakeShared {
  std::mutex lock;
  std::vector<std::vector<int>> nal_types;  // per access unit
  int resets = 0;
};

class FakeBackend : public video::DecoderBackend {
 public:
  FakeBackend(int w, int h, std::shared_ptr<FakeShared> shared)
      : w_(w), h_(h), shared_(std::move(shared)), buffers_(8) {
    for (auto& b : buffers_) {
      b.resize(static_cast<std::size_t>(w) * h * 3 / 2);
    }
  }
  int slot_count() const override { return 8; }
  int max_width() const override { return w_; }
  int max_height() const override { return h_; }

  Result decode(const std::uint8_t* au, std::size_t size, std::int64_t pts, int target) override {
    {
      std::lock_guard<std::mutex> lock(shared_->lock);
      shared_->nal_types.push_back(video::annexb_nal_types(VideoCodec::H264, au, size));
    }
    Bytes& b = buffers_[static_cast<std::size_t>(target)];
    std::fill(b.begin(), b.begin() + w_ * h_, static_cast<std::uint8_t>(16 + pts % 200));
    std::fill(b.begin() + w_ * h_, b.end(), 128);
    Result r;
    r.target_taken = true;
    r.frame_slot = pending_;
    if (pending_ >= 0) {
      r.picture = view(pending_);
    }
    pending_ = target;
    return r;
  }
  Result flush(int) override {
    Result r;
    r.frame_slot = pending_;
    if (pending_ >= 0) {
      r.picture = view(pending_);
    }
    pending_ = -1;
    return r;
  }
  bool reset() override {
    pending_ = -1;
    std::lock_guard<std::mutex> lock(shared_->lock);
    shared_->resets++;
    return true;
  }

 private:
  video::Nv12View view(int slot) const {
    const Bytes& b = buffers_[static_cast<std::size_t>(slot)];
    return {b.data(), b.data() + w_ * h_, w_, w_, w_, h_};
  }
  int w_, h_;
  std::shared_ptr<FakeShared> shared_;
  std::vector<Bytes> buffers_;
  int pending_ = -1;
};

std::uint8_t expected_grey(std::int64_t pts) {
  const int y = static_cast<int>(16 + pts % 200);
  return static_cast<std::uint8_t>(std::lround((y - 16) * 255.0 / 219.0));
}

}  // namespace

TEST_SUITE("hwdec_e2e") {
  TEST_CASE("packets in, pictures announced, the shown one reaches the overlay") {
    auto shared = std::make_shared<FakeShared>();
    video::ServerOptions options;
    options.port = 0;
    options.stats_interval_ms = 0;
    options.decoder_factory = [shared](const video::DecoderRequest& request, std::string&) {
      return std::make_unique<FakeBackend>(request.width, request.height, shared);
    };
    video::VideoServer server(options);
    REQUIRE(server.start());

    wire::VideoOverlayClient client;
    wire::VideoOverlayClient::Config config;
    config.port = server.port();
    config.pid = static_cast<std::uint32_t>(getpid());
    config.player = 3;
    client.start(config);
    REQUIRE(wait_until([&] { return client.usable(); }));
    const video::PageKey page{config.pid, 5};
    client.setGeometry(wire::Geometry{5, 0, 0, 16, 8, true, false});

    wire::StreamOpen open;
    open.codec = wire::Codec::H264;
    open.width = 16;
    open.height = 8;
    open.extradata = avcc_record();
    auto stream = client.openStream(open);
    std::atomic<bool> abort{false};
    REQUIRE(stream->waitForAnswer(2, abort) == wire::RemoteVideoStream::State::Hardware);
    CHECK(stream->maxFrames() == 8);
    CHECK(server.decode_stream_count() == 1);

    // A non-keyframe first: dropped until the keyframe.
    const Bytes delta = length_prefixed({kSlice});
    const Bytes key = length_prefixed({kIdr});
    CHECK(stream->sendPacket({0, 0, 0, 0, false}, delta.data(), delta.size()));
    for (int i = 0; i < 5; ++i) {
      const Bytes& au = i == 0 ? key : delta;
      CHECK(stream->sendPacket({0, 0, 100 + i, 100 + i, i == 0}, au.data(), au.size()));
    }
    // Four pictures out (one stays inside the fake decoder).
    std::vector<wire::Decoded> frames;
    REQUIRE(wait_until([&] {
      for (auto& n : stream->takeNotices()) {
        frames.push_back(n);
      }
      return frames.size() >= 4;
    }));
    CHECK(frames[0].pts == 100);
    CHECK(frames[3].pts == 103);
    CHECK(frames[0].width == 16);
    REQUIRE(wait_until([&] { return stream->packetsInFlight() == 0; }));
    {
      std::lock_guard<std::mutex> lock(shared->lock);
      REQUIRE(shared->nal_types.size() == 5);  // the leading delta never reached it
      CHECK(shared->nal_types[0] == std::vector<int>{7, 8, 5});
      CHECK(shared->nal_types[1] == std::vector<int>{1});
    }

    // Show 102: the overlay gets that picture (grey of pts 102).
    client.showRemote(stream->id(), 0, 102);
    std::uint32_t pixel = 0;
    REQUIRE(wait_until([&] {
      auto f = server.acquire(page);
      if (f.layers().size() != 1) {
        return false;
      }
      pixel = f.layers()[0].pixels[0];
      return channel(pixel, 0) == expected_grey(102);
    }));
    CHECK(channel(pixel, 8) == expected_grey(102));
    CHECK(channel(pixel, 24) == 0xFF);

    // End of stream: the last picture, then "drained".
    stream->drain(0);
    bool drained = false;
    REQUIRE(wait_until([&] {
      for (auto& n : stream->takeNotices()) {
        if (n.hasFrame()) {
          CHECK(n.pts == 104);
        }
        drained = drained || n.drained();
      }
      return drained;
    }));

    // A seek: flush to serial 1, then keyframe first again.
    stream->flush(1);
    CHECK(stream->sendPacket({0, 1, 500, 500, true}, key.data(), key.size()));
    CHECK(stream->sendPacket({0, 1, 501, 501, false}, delta.data(), delta.size()));
    std::vector<wire::Decoded> after;
    REQUIRE(wait_until([&] {
      for (auto& n : stream->takeNotices()) {
        after.push_back(n);
      }
      return !after.empty();
    }));
    CHECK(after[0].serial == 1);
    CHECK(after[0].pts == 500);
    {
      std::lock_guard<std::mutex> lock(shared->lock);
      CHECK(shared->resets >= 2);  // drain, flush
    }
    client.showRemote(stream->id(), 1, 500);
    REQUIRE(wait_until([&] {
      auto f = server.acquire(page);
      return f.layers().size() == 1 && channel(f.layers()[0].pixels[0], 0) == expected_grey(500);
    }));

    stream->close("done");
    REQUIRE(wait_until([&] { return server.decode_stream_count() == 0; }));
    client.stop();
    server.stop();
  }

  TEST_CASE("refusals: no decoder, unsupported codec, no server") {
    video::ServerOptions options;
    options.port = 0;
    options.stats_interval_ms = 0;
    video::VideoServer server(options);  // no decoder_factory
    REQUIRE(server.start());
    wire::VideoOverlayClient client;
    wire::VideoOverlayClient::Config config;
    config.port = server.port();
    config.player = 4;
    client.start(config);
    REQUIRE(wait_until([&] { return client.usable(); }));
    std::atomic<bool> abort{false};

    wire::StreamOpen open;
    open.codec = wire::Codec::H264;
    open.width = 1920;
    open.height = 1080;
    auto stream = client.openStream(open);
    CHECK(stream->waitForAnswer(2, abort) == wire::RemoteVideoStream::State::Rejected);
    CHECK(stream->reason() == "no hardware decoder in this build");
    const Bytes data = {0, 0, 0, 1, 0x65};
    CHECK_FALSE(stream->sendPacket({0, 0, 0, 0, true}, data.data(), data.size()));

    open.codec = wire::Codec::Vp9;
    auto vp9 = client.openStream(open);
    CHECK(vp9->waitForAnswer(2, abort) == wire::RemoteVideoStream::State::Rejected);
    CHECK(vp9->reason() == "unsupported");
    client.stop();
    // A stream of a stopped client fails at once.
    CHECK(client.openStream(open)->state() == wire::RemoteVideoStream::State::Failed);
    server.stop();

    wire::VideoOverlayClient lonely;
    wire::VideoOverlayClient::Config none;
    none.port = 1;
    none.connectTimeoutMs = 100;
    lonely.start(none);
    auto orphan = lonely.openStream(open);
    CHECK(orphan->waitForAnswer(2, abort) == wire::RemoteVideoStream::State::Failed);
    lonely.stop();
  }

  TEST_CASE("credit: at most maxPacketsInFlight packets wait for the decoder") {
    wire::RemoteVideoStream stream(1);
    CHECK(stream.hasCredit(1u << 30));  // the first packet always goes
    CHECK(stream.packetsInFlight() == 0);
  }
}

namespace {

// Bit writer for parameter sets (Exp-Golomb), RBSP trailing bits included.
class BitWriter {
 public:
  void bits(std::uint32_t value, int n) {
    for (int i = n - 1; i >= 0; --i) {
      bit((value >> i) & 1);
    }
  }
  void ue(std::uint32_t value) {
    const std::uint32_t v = value + 1;
    int len = 0;
    while ((v >> len) > 1) {
      ++len;
    }
    bits(0, len);
    bits(v, len + 1);
  }
  Bytes finish() {
    bit(1);
    while (count_ % 8) {
      bit(0);
    }
    // Emulation prevention, as an encoder writes it.
    Bytes out;
    int zeros = 0;
    for (std::uint8_t b : bytes_) {
      if (zeros >= 2 && b <= 3) {
        out.push_back(3);
        zeros = 0;
      }
      out.push_back(b);
      zeros = b == 0 ? zeros + 1 : 0;
    }
    return out;
  }

 private:
  void bit(std::uint32_t b) {
    if (count_ % 8 == 0) {
      bytes_.push_back(0);
    }
    bytes_.back() |= static_cast<std::uint8_t>(b << (7 - count_ % 8));
    ++count_;
  }
  Bytes bytes_;
  int count_ = 0;
};

// An H.264 High profile SPS of width x height (macroblock aligned, cropped).
Bytes h264_sps(int width, int height) {
  BitWriter w;
  w.bits(0x67, 8);
  w.bits(100, 8);  // profile_idc
  w.bits(0, 8);
  w.bits(40, 8);  // level_idc
  w.ue(0);        // seq_parameter_set_id
  w.ue(1);        // chroma_format_idc 4:2:0
  w.ue(0);
  w.ue(0);
  w.bits(0, 1);
  w.bits(0, 1);  // no scaling matrix
  w.ue(0);       // log2_max_frame_num_minus4
  w.ue(0);       // pic_order_cnt_type
  w.ue(0);       // log2_max_pic_order_cnt_lsb_minus4
  w.ue(4);       // max_num_ref_frames
  w.bits(0, 1);
  const int mbs_w = (width + 15) / 16;
  const int mbs_h = (height + 15) / 16;
  w.ue(static_cast<std::uint32_t>(mbs_w - 1));
  w.ue(static_cast<std::uint32_t>(mbs_h - 1));
  w.bits(1, 1);  // frame_mbs_only_flag
  w.bits(1, 1);  // direct_8x8_inference_flag
  const int crop_r = (mbs_w * 16 - width) / 2;
  const int crop_b = (mbs_h * 16 - height) / 2;
  w.bits(crop_r || crop_b ? 1 : 0, 1);
  if (crop_r || crop_b) {
    w.ue(0);
    w.ue(static_cast<std::uint32_t>(crop_r));
    w.ue(0);
    w.ue(static_cast<std::uint32_t>(crop_b));
  }
  w.bits(0, 1);  // vui_parameters_present_flag
  return w.finish();
}

// An HEVC Main SPS of width x height (coded in 8-pixel units, conformance
// window for the rest).
Bytes hevc_sps(int width, int height) {
  BitWriter w;
  w.bits(0x4201, 16);
  w.bits(0, 4);  // sps_video_parameter_set_id
  w.bits(0, 3);  // sps_max_sub_layers_minus1
  w.bits(1, 1);
  w.bits(0, 2);
  w.bits(0, 1);
  w.bits(1, 5);  // general_profile_idc Main
  w.bits(0x60000000u, 32);
  w.bits(0, 4);
  w.bits(0, 32);
  w.bits(0, 11);
  w.bits(0, 1);
  w.bits(120, 8);  // general_level_idc
  w.ue(0);         // sps_seq_parameter_set_id
  w.ue(1);         // chroma_format_idc
  const int coded_w = (width + 7) / 8 * 8;
  const int coded_h = (height + 7) / 8 * 8;
  w.ue(static_cast<std::uint32_t>(coded_w));
  w.ue(static_cast<std::uint32_t>(coded_h));
  const bool window = coded_w != width || coded_h != height;
  w.bits(window ? 1 : 0, 1);
  if (window) {
    w.ue(0);
    w.ue(static_cast<std::uint32_t>((coded_w - width) / 2));
    w.ue(0);
    w.ue(static_cast<std::uint32_t>((coded_h - height) / 2));
  }
  w.bits(0, 1);
  return w.finish();
}

// avcC carrying a real SPS.
Bytes avcc_with_sps(const Bytes& sps) {
  Bytes r = {1, 0x64, 0x00, 0x28, 0xFF, 0xE1};
  r.push_back(static_cast<std::uint8_t>(sps.size() >> 8));
  r.push_back(static_cast<std::uint8_t>(sps.size()));
  r.insert(r.end(), sps.begin(), sps.end());
  r.push_back(1);
  r.push_back(0);
  r.push_back(static_cast<std::uint8_t>(kPps.size()));
  r.insert(r.end(), kPps.begin(), kPps.end());
  r.insert(r.end(), {0xFD, 0xF8, 0xF8, 0});
  return r;
}

// Floating-point bilinear with the scaler's sample positions (pixel
// centres, clamped at the edges).
std::vector<std::uint32_t> bilinear_reference(const std::vector<std::uint32_t>& src, int sw, int sh,
                                              int dw, int dh) {
  std::vector<std::uint32_t> out(static_cast<std::size_t>(dw) * dh);
  auto coord = [](int i, int s, int d, int& i0, int& i1, double& f) {
    double p = (i + 0.5) * s / d - 0.5;
    p = std::clamp(p, 0.0, static_cast<double>(s - 1));
    i0 = static_cast<int>(p);
    i1 = std::min(i0 + 1, s - 1);
    f = p - i0;
  };
  for (int y = 0; y < dh; ++y) {
    int y0, y1;
    double fy;
    coord(y, sh, dh, y0, y1, fy);
    for (int x = 0; x < dw; ++x) {
      int x0, x1;
      double fx;
      coord(x, sw, dw, x0, x1, fx);
      std::uint32_t pixel = 0;
      for (int shift : {0, 8, 16, 24}) {
        auto c = [&](int xx, int yy) {
          return static_cast<double>((src[static_cast<std::size_t>(yy) * sw + xx] >> shift) & 0xFF);
        };
        const double top = c(x0, y0) * (1 - fx) + c(x1, y0) * fx;
        const double bottom = c(x0, y1) * (1 - fx) + c(x1, y1) * fx;
        const auto v = static_cast<std::uint32_t>(std::lround(top * (1 - fy) + bottom * fy));
        pixel |= std::min<std::uint32_t>(v, 255) << shift;
      }
      out[static_cast<std::size_t>(y) * dw + x] = pixel;
    }
  }
  return out;
}

int max_channel_diff(const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
  int worst = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    for (int shift : {0, 8, 16, 24}) {
      worst = std::max(worst, std::abs(channel(a[i], shift) - channel(b[i], shift)));
    }
  }
  return worst;
}

}  // namespace

TEST_SUITE("hwdec_geometry") {
  TEST_CASE("SPS visible size: H.264 cropping and the HEVC conformance window") {
    int w = 0;
    int h = 0;
    const Bytes avc1080 = h264_sps(1920, 1080);
    REQUIRE(video::sps_visible_size(VideoCodec::H264, avc1080.data(), avc1080.size(), w, h));
    CHECK(w == 1920);
    CHECK(h == 1080);
    const Bytes avc720 = h264_sps(1280, 720);
    REQUIRE(video::sps_visible_size(VideoCodec::H264, avc720.data(), avc720.size(), w, h));
    CHECK(w == 1280);
    CHECK(h == 720);
    const Bytes hevc = hevc_sps(1920, 1080);
    REQUIRE(video::sps_visible_size(VideoCodec::Hevc, hevc.data(), hevc.size(), w, h));
    CHECK(w == 1920);
    CHECK(h == 1080);
    CHECK_FALSE(video::sps_visible_size(VideoCodec::H264, kSps.data(), kSps.size(), w, h));

    // parse_stream_config reports it (avcC and Annex-B extradata).
    std::string error;
    const Bytes record = avcc_with_sps(avc1080);
    auto config = video::parse_stream_config(VideoCodec::H264, record.data(), record.size(), error);
    REQUIRE(config);
    CHECK(config->width == 1920);
    CHECK(config->height == 1080);
    const Bytes ab = annexb({avc720, kPps});
    config = video::parse_stream_config(VideoCodec::H264, ab.data(), ab.size(), error);
    REQUIRE(config);
    CHECK(config->width == 1280);
    CHECK(config->height == 720);
  }

  TEST_CASE("NV12 with a padded pitch and coded rows converts only the visible picture") {
    // 1080p-like layout, scaled down: visible 38x18, coded 48x32 rows, pitch
    // 64 bytes, the chroma plane after the coded rows. Padding is garbage.
    const int vw = 38, vh = 18, coded_h = 32, pitch = 64;
    std::mt19937 rng(7);
    Bytes frame(static_cast<std::size_t>(pitch) * coded_h * 3 / 2);
    for (auto& b : frame) {
      b = static_cast<std::uint8_t>(rng());
    }
    // The same visible picture, packed (pitch == width).
    Bytes packed_y(static_cast<std::size_t>(vw) * vh);
    Bytes packed_uv(static_cast<std::size_t>(vw) * vh / 2);
    for (int y = 0; y < vh; ++y) {
      std::memcpy(&packed_y[static_cast<std::size_t>(y) * vw], &frame[static_cast<std::size_t>(y) * pitch], vw);
    }
    const std::uint8_t* uv = frame.data() + static_cast<std::size_t>(pitch) * coded_h;
    for (int y = 0; y < vh / 2; ++y) {
      std::memcpy(&packed_uv[static_cast<std::size_t>(y) * vw], uv + static_cast<std::size_t>(y) * pitch, vw);
    }
    const video::Nv12View padded{frame.data(), uv, pitch, pitch, vw, vh};
    const video::Nv12View packed{packed_y.data(), packed_uv.data(), vw, vw, vw, vh};
    const video::YuvColor color{video::YuvMatrix::Bt709, false};
    std::vector<std::int32_t> scratch;
    std::vector<std::uint32_t> from_padded(static_cast<std::size_t>(vw) * vh);
    std::vector<std::uint32_t> from_packed(from_padded.size());
    std::vector<std::uint32_t> reference(from_padded.size());
    video::nv12_to_argb(padded, color, 1, from_padded.data(), vw, scratch);
    video::nv12_to_argb(packed, color, 1, from_packed.data(), vw, scratch);
    video::nv12_to_argb_reference(packed, color, 1, reference.data(), vw);
    CHECK(from_padded == from_packed);
    CHECK(max_channel_diff(from_padded, reference) <= 1);
  }

  TEST_CASE("the converted picture scales to an arbitrary rect like the reference") {
    std::mt19937 rng(3);
    const int sw = 64, sh = 36;
    std::vector<std::uint32_t> src(static_cast<std::size_t>(sw) * sh);
    // Smooth content (bilinear's 8-bit weights stay within 1-2 per channel).
    for (int y = 0; y < sh; ++y) {
      for (int x = 0; x < sw; ++x) {
        src[static_cast<std::size_t>(y) * sw + x] =
            0xFF000000u | static_cast<std::uint32_t>(x * 4) << 16 |
            static_cast<std::uint32_t>(y * 7) << 8 | static_cast<std::uint32_t>((x + y) * 2);
      }
    }
    const display::ImageView view{src.data(), sw, sh, sw};
    std::vector<std::uint32_t> scratch;
    for (auto [dw, dh] : {std::pair{96, 54}, {1399 / 8, 787 / 8}, {37, 21}, {150, 31}}) {
      std::vector<std::uint32_t> got(static_cast<std::size_t>(dw) * dh);
      display::scale_image(view, got.data(), dw, dh, dw, display::ScaleFilter::Bilinear, scratch);
      CHECK(max_channel_diff(got, bilinear_reference(src, sw, sh, dw, dh)) <= 2);
    }
  }

  TEST_CASE("slot wait policy: wait for the player, never evict frames it will show") {
    video::SlotWaitPolicy policy;
    using Action = video::SlotWaitPolicy::Action;
    policy.start(1000, 50);
    // Paused (no shows) for a minute: keep waiting.
    CHECK(policy.on_timeout(61000, 50, false, 0) == Action::Wait);
    // Shows arriving, the clock trailing the decoder: wait.
    CHECK(policy.on_timeout(5000, 80, false, 0) == Action::Wait);
    // Late pictures behind the one on screen go first.
    CHECK(policy.on_timeout(1500, 50, false, 2) == Action::ReleaseStale);
    // Hidden: nothing is presented, everything held goes.
    CHECK(policy.on_timeout(1500, 50, true, 0) == Action::ReleaseHeld);
    // Shows for 10 s without freeing anything: those pictures leaked.
    CHECK(policy.on_timeout(1000 + video::SlotWaitPolicy::kLeakMs, 90, false, 0) ==
          Action::EvictOldest);

    video::FrameSlots slots(4);
    slots.hold(0, 100);
    slots.hold(1, 200);
    CHECK(slots.show(200).slot == 1);
    slots.hold(2, 150);  // came after the show of a later picture
    slots.hold(3, 300);
    CHECK(slots.stale_count() == 1);
    CHECK(slots.release_stale() == 1);
    CHECK(slots.held_count() == 1);  // 300 stays: it is shown next
  }
}

TEST_SUITE("hwdec_visibility") {
  struct Harness {
    std::shared_ptr<FakeShared> shared = std::make_shared<FakeShared>();
    std::mutex lock;
    std::vector<wire::Decoded> notices;
    std::atomic<int> delivered{0};
    std::atomic<int> last_grey{-1};
    std::unique_ptr<video::HwStream> stream;

    explicit Harness(const Bytes& record) {
      std::string error;
      auto config = video::parse_stream_config(VideoCodec::H264, record.data(), record.size(), error);
      REQUIRE(config);
      video::HwStreamCallbacks callbacks;
      callbacks.send = [this](std::vector<std::uint8_t>&& bytes) {
        wire::Parser parser;
        parser.append(bytes.data(), bytes.size());
        while (parser.next() == wire::Parser::Result::Message) {
          const auto& m = parser.message();
          if (m.type == wire::Type::Decoded) {
            if (auto d = wire::decodeDecoded(m.payload, m.bytes); d && d->hasFrame()) {
              std::lock_guard<std::mutex> guard(lock);
              notices.push_back(*d);
            }
          }
        }
      };
      callbacks.deliver = [this](const display::ImageView& view) {
        last_grey = channel(view.pixels[0], 0);
        delivered++;
      };
      stream = std::make_unique<video::HwStream>(
          1, 1, std::move(*config), video::YuvColor{video::YuvMatrix::Bt709, false},
          std::make_unique<FakeBackend>(16, 8, shared), std::move(callbacks));
    }
    void send(std::int64_t pts, bool key) {
      const Bytes au = length_prefixed({key ? kIdr : kSlice});
      stream->packet(0, pts, key, au.data(), au.size());
    }
    std::size_t decoded() {
      std::lock_guard<std::mutex> guard(lock);
      return notices.size();
    }
  };

  TEST_CASE("a paused player: the decoder waits for it instead of evicting the next frames") {
    Harness h(avcc_record());
    // 8 slots: 12 pictures cannot all be held; nobody shows (paused).
    for (int i = 0; i < 12; ++i) {
      h.send(100 + i, i == 0);
    }
    REQUIRE(wait_until([&] { return h.decoded() >= 7; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));  // past two slot waits
    CHECK(h.stream->held_pictures() >= 7);
    // Playback resumes: the first frame is still there to be shown.
    h.stream->show(0, 100);
    REQUIRE(wait_until([&] { return h.last_grey.load() == expected_grey(100); }));
    // ... and the decoder goes on as the player shows the rest.
    for (int i = 1; i < 11; ++i) {
      h.stream->show(0, 100 + i);
      REQUIRE(wait_until([&] { return h.last_grey.load() == expected_grey(100 + i); }));
    }
  }

  TEST_CASE("hidden: held pictures freed, no presenting; shown: the picture comes back") {
    Harness h(avcc_record());
    for (int i = 0; i < 4; ++i) {
      h.send(200 + i, i == 0);
    }
    REQUIRE(wait_until([&] { return h.decoded() >= 3; }));
    h.stream->show(0, 200);
    REQUIRE(wait_until([&] { return h.last_grey.load() == expected_grey(200); }));
    CHECK(h.stream->held_pictures() == 2);

    h.stream->set_visible(false);
    CHECK(h.stream->hidden());
    CHECK(h.stream->held_pictures() == 0);
    // Pictures decoded while hidden never block the decoder.
    for (int i = 4; i < 20; ++i) {
      h.send(200 + i, false);
    }
    REQUIRE(wait_until([&] { return h.decoded() >= 18; }, 5000));

    // Shown again: the picture on screen is presented at once ...
    const int before = h.delivered.load();
    h.stream->set_visible(true);
    CHECK_FALSE(h.stream->hidden());
    REQUIRE(wait_until([&] { return h.delivered.load() > before; }));
    CHECK(h.last_grey.load() == expected_grey(200));
    // ... and the player's flush on the resume keyframe starts it over,
    // the next show presenting the new picture.
    h.stream->flush(0);
    h.send(500, true);
    h.send(501, false);
    REQUIRE(wait_until([&] {
      std::lock_guard<std::mutex> guard(h.lock);
      return !h.notices.empty() && h.notices.back().pts == 500;
    }));
    h.stream->show(0, 500);
    REQUIRE(wait_until([&] { return h.last_grey.load() == expected_grey(500); }));
  }

  TEST_CASE("pictures are cropped to the parameter sets' visible size, not the container's") {
    // The fake decoder outputs 16x16 pictures; the SPS says 16x10 visible.
    std::string error;
    const Bytes record = avcc_with_sps(h264_sps(16, 10));
    auto config = video::parse_stream_config(VideoCodec::H264, record.data(), record.size(), error);
    REQUIRE(config);
    std::mutex lock;
    std::vector<std::pair<int, int>> sizes;
    video::HwStreamCallbacks callbacks;
    callbacks.send = [&](std::vector<std::uint8_t>&& bytes) {
      wire::Parser parser;
      parser.append(bytes.data(), bytes.size());
      while (parser.next() == wire::Parser::Result::Message) {
        const auto& m = parser.message();
        if (m.type != wire::Type::Decoded) {
          continue;
        }
        if (auto d = wire::decodeDecoded(m.payload, m.bytes); d && d->hasFrame()) {
          std::lock_guard<std::mutex> guard(lock);
          sizes.emplace_back(static_cast<int>(d->width), static_cast<int>(d->height));
        }
      }
    };
    auto shared = std::make_shared<FakeShared>();
    video::HwStream stream(1, 1, std::move(*config), video::YuvColor{},
                           std::make_unique<FakeBackend>(16, 16, shared), std::move(callbacks));
    const Bytes key = length_prefixed({kIdr});
    const Bytes delta = length_prefixed({kSlice});
    stream.packet(0, 10, true, key.data(), key.size());
    stream.packet(0, 11, false, delta.data(), delta.size());
    REQUIRE(wait_until([&] {
      std::lock_guard<std::mutex> guard(lock);
      return !sizes.empty();
    }));
    std::lock_guard<std::mutex> guard(lock);
    CHECK(sizes[0] == std::pair{16, 10});
  }
}
