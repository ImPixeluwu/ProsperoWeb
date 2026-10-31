// SPDX-License-Identifier: GPL-3.0-or-later
//
// Video overlay: the wire protocol and shared-memory seqlock
// (FFmpegVideoOverlayWire.h), the view -> window transforms, the scaler
// against a floating-point reference, composition of the page over a video
// layer, and a loopback run of the WebProcess client against the UI server
// over each transport (shm, file, tcp).
#include <chrono>
#include <cstdint>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <doctest/doctest.h>
#include <unistd.h>

#include "FFmpegVideoOverlayClient.h"
#include "FFmpegVideoOverlayWire.h"
#include "compose.h"
#include "scale.h"
#include "video_geometry.h"
#include "video_server.h"

namespace wire = WebCore::FFmpegMedia::VideoWire;
using namespace prosperobrowser;
using display::Rect;

namespace {

std::vector<std::uint32_t> random_image(int w, int h, std::uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<std::uint32_t> img(static_cast<std::size_t>(w) * h);
  for (auto& p : img) {
    p = rng() | 0xFF000000u;
  }
  return img;
}

int max_channel_diff(std::uint32_t a, std::uint32_t b) {
  int worst = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    worst = std::max(worst, std::abs(static_cast<int>((a >> shift) & 0xFF) -
                                     static_cast<int>((b >> shift) & 0xFF)));
  }
  return worst;
}

template <typename F>
bool wait_until(F&& condition, int timeout_ms = 3000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return condition();
}

}  // namespace

TEST_SUITE("video_wire") {
  TEST_CASE("hello, geometry and present round-trip") {
    std::uint8_t hello[wire::helloSize];
    wire::encodeHello(wire::Hello{wire::version, 4242, 7}, hello);
    auto decoded = wire::decodeHello(hello);
    REQUIRE(decoded);
    CHECK(decoded->pid == 4242);
    CHECK(decoded->player == 7);
    hello[0] = 'X';
    CHECK_FALSE(wire::decodeHello(hello));

    std::vector<std::uint8_t> out;
    wire::Geometry g{0x123456789ull, -20, 96, 1280, 720, true, false};
    wire::appendGeometry(out, g);
    REQUIRE(out.size() == wire::headerSize + wire::geometryBytes);
    auto g2 = wire::decodeGeometry(out.data() + wire::headerSize, wire::geometryBytes);
    REQUIRE(g2);
    CHECK(*g2 == g);

    out.clear();
    wire::Present p{2, 10, {1280, 720, 5120, 12.5}};
    wire::appendPresent(out, p);
    auto p2 = wire::decodePresent(out.data() + wire::headerSize, out.size() - wire::headerSize);
    REQUIRE(p2);
    CHECK(p2->slot == 2);
    CHECK(p2->seq == 10);
    CHECK(p2->frame.stride == 5120);
    CHECK(p2->frame.pts == doctest::Approx(12.5));
  }

  TEST_CASE("shm offers and frames are validated") {
    std::vector<std::uint8_t> out;
    wire::appendShmOffer(out, {wire::ShmKind::File, 3, 4096, "/tmp/x.shm"});
    auto offer = wire::decodeShmOffer(out.data() + wire::headerSize, out.size() - wire::headerSize);
    REQUIRE(offer);
    CHECK(offer->kind == wire::ShmKind::File);
    CHECK(offer->name == "/tmp/x.shm");
    out.clear();
    wire::appendShmOffer(out, {wire::ShmKind::Named, 0, 4096, "/a"});
    CHECK_FALSE(wire::decodeShmOffer(out.data() + wire::headerSize, out.size() - wire::headerSize));

    wire::FrameInfo f{4, 2, 16, 0};
    CHECK(f.valid(32));
    CHECK_FALSE(f.valid(31));
    wire::FrameInfo narrow{4, 2, 12, 0};  // stride shorter than a row
    CHECK_FALSE(narrow.valid(1024));
    std::vector<std::uint8_t> payload(wire::frameHeaderBytes + 32);
    wire::encodeFrameHeader(f, payload.data());
    const std::uint8_t* pixels = nullptr;
    CHECK(wire::decodeFrame(payload.data(), payload.size(), &pixels));
    CHECK(pixels == payload.data() + wire::frameHeaderBytes);
    CHECK_FALSE(wire::decodeFrame(payload.data(), payload.size() - 1, &pixels));
  }

  TEST_CASE("the parser reassembles split messages") {
    std::vector<std::uint8_t> stream(wire::helloSize);
    wire::encodeHello(wire::Hello{wire::version, 1, 2}, stream.data());
    wire::appendGeometry(stream, wire::Geometry{9, 1, 2, 3, 4, true, true});
    wire::appendMessage(stream, wire::Type::Clear);
    wire::Parser parser(/*expectHello=*/true);
    std::vector<wire::Parser::Result> results;
    for (std::uint8_t byte : stream) {  // one byte at a time
      parser.append(&byte, 1);
      for (auto r = parser.next(); r != wire::Parser::Result::NeedMore; r = parser.next()) {
        results.push_back(r);
        if (r == wire::Parser::Result::Message && parser.message().type == wire::Type::Geometry) {
          auto g = wire::decodeGeometry(parser.message().payload, parser.message().bytes);
          REQUIRE(g);
          CHECK(g->pageId == 9);
          CHECK(g->fullscreen);
        }
      }
    }
    REQUIRE(results.size() == 3);
    CHECK(results[0] == wire::Parser::Result::Hello);
    CHECK(parser.hello().player == 2);
    CHECK(parser.pendingBytes() == 0);

    // missingBytes() says how much of a large message is still to come.
    wire::Parser p2;
    std::vector<std::uint8_t> frame;
    wire::appendMessage(frame, wire::Type::Frame, std::vector<std::uint8_t>(1000).data(), 1000);
    p2.append(frame.data(), 100);
    CHECK(p2.missingBytes() == frame.size() - 100);
    CHECK(p2.next() == wire::Parser::Result::NeedMore);
    std::uint8_t* w = p2.writable(frame.size() - 100);
    std::memcpy(w, frame.data() + 100, frame.size() - 100);
    p2.commit(frame.size() - 100);
    CHECK(p2.next() == wire::Parser::Result::Message);
    CHECK(p2.message().bytes == 1000);
  }

  TEST_CASE("the slot seqlock rejects torn and stale copies") {
    const wire::ShmLayout layout{3, 4096};
    std::vector<std::uint8_t> segment(layout.totalBytes());
    layout.writeHeader(segment.data());
    CHECK(layout.matches(segment.data(), segment.size()));
    CHECK_FALSE((wire::ShmLayout{3, 8192}).matches(segment.data(), segment.size()));

    wire::SlotWriter writer(segment.data(), 1);
    // While the writer holds the slot its counter is odd.
    CHECK((wire::ShmLayout::sequence(segment.data(), 1).load() & 1) == 1);
    std::memset(segment.data() + layout.slotOffset(1), 0xAB, 64);
    const std::uint32_t seq = writer.finish();
    CHECK((seq & 1) == 0);
    std::vector<std::uint8_t> out(64);
    CHECK(wire::readSlot(segment.data(), layout, 1, seq, out.data(), out.size()));
    CHECK(out[63] == 0xAB);
    // Rewritten since the announcement: the copy is stale.
    wire::SlotWriter again(segment.data(), 1);
    CHECK_FALSE(wire::readSlot(segment.data(), layout, 1, seq, out.data(), out.size()));
    const std::uint32_t seq2 = again.finish();
    CHECK(seq2 == seq + 2);
    CHECK_FALSE(wire::readSlot(segment.data(), layout, 1, seq, out.data(), out.size()));
    CHECK(wire::readSlot(segment.data(), layout, 1, seq2, out.data(), out.size()));
    CHECK_FALSE(wire::readSlot(segment.data(), layout, 3, seq2, out.data(), out.size()));
  }
}

TEST_SUITE("video_geometry") {
  TEST_CASE("view rects map below the chrome band and are clipped to the content") {
    CHECK(video::view_to_window(Rect{100, 50, 640, 360}, 96) == Rect{100, 146, 640, 360});
    CHECK(video::content_area(96, 1920, 1080) == Rect{0, 96, 1920, 984});
    // Scrolled half out of the top of the view: only the visible part is damaged.
    auto damage = video::damage_to_window({Rect{0, -100, 400, 300}}, 96, 1920, 1080);
    REQUIRE(damage.rects().size() == 1);
    CHECK(damage.rects()[0] == Rect{0, 96, 400, 200});
    // Fullscreen: no band, the whole window.
    auto full = video::damage_to_window({Rect{0, 0, 1920, 1080}}, 0, 1920, 1080);
    REQUIRE(full.rects().size() == 1);
    CHECK(full.rects()[0] == Rect{0, 0, 1920, 1080});
    // Entirely outside the view: nothing.
    CHECK(video::damage_to_window({Rect{0, 2000, 10, 10}}, 96, 1920, 1080).empty());
  }
}

TEST_SUITE("video_scale") {
  TEST_CASE("same size copies and forces alpha") {
    std::vector<std::uint32_t> src = {0x00112233u, 0x80445566u, 0xFF778899u, 0x01AABBCCu};
    std::vector<std::uint32_t> dst(4), scratch;
    display::scale_image({src.data(), 2, 2, 2}, dst.data(), 2, 2, 2,
                         display::ScaleFilter::Bilinear, scratch);
    CHECK(dst[0] == 0xFF112233u);
    CHECK(dst[3] == 0xFFAABBCCu);
  }

  TEST_CASE("bilinear matches the reference within rounding") {
    struct Case {
      int sw, sh, dw, dh;
    };
    for (const Case& c : {Case{64, 36, 160, 90}, Case{160, 90, 64, 36}, Case{33, 17, 100, 51},
                          Case{1280, 720, 1920, 1080}, Case{100, 100, 37, 211}}) {
      const auto src = random_image(c.sw, c.sh, static_cast<std::uint32_t>(c.sw * 31 + c.dh));
      std::vector<std::uint32_t> got(static_cast<std::size_t>(c.dw) * c.dh);
      std::vector<std::uint32_t> want(got.size()), scratch;
      display::scale_image({src.data(), c.sw, c.sh, c.sw}, got.data(), c.dw, c.dh, c.dw,
                           display::ScaleFilter::Bilinear, scratch);
      display::scale_image_reference({src.data(), c.sw, c.sh, c.sw}, want.data(), c.dw, c.dh,
                                     c.dw);
      int worst = 0;
      for (std::size_t i = 0; i < got.size(); i++) {
        worst = std::max(worst, max_channel_diff(got[i], want[i]));
      }
      INFO(c.sw << "x" << c.sh << " -> " << c.dw << "x" << c.dh);
      CHECK(worst <= 2);
      CHECK((got[0] >> 24) == 0xFF);
    }
  }

  TEST_CASE("nearest doubles pixels exactly; flat images stay flat") {
    std::vector<std::uint32_t> src = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu};
    std::vector<std::uint32_t> dst(16), scratch;
    display::scale_image({src.data(), 2, 2, 2}, dst.data(), 4, 4, 4, display::ScaleFilter::Nearest,
                         scratch);
    CHECK(dst[0] == 0xFF0000FFu);
    CHECK(dst[1] == 0xFF0000FFu);
    CHECK(dst[2] == 0xFF00FF00u);
    CHECK(dst[15] == 0xFFFFFFFFu);
    std::vector<std::uint32_t> flat(50 * 30, 0xFF336699u), out(77 * 41);
    display::scale_image({flat.data(), 50, 30, 50}, out.data(), 77, 41, 77,
                         display::ScaleFilter::Bilinear, scratch);
    bool all_flat = true;
    for (std::uint32_t p : out) {
      all_flat = all_flat && p == 0xFF336699u;
    }
    CHECK(all_flat);
  }
}

TEST_SUITE("video_compose") {
  TEST_CASE("the video shows through the page's hole, under captions, inside the clip") {
    const int w = 8, h = 4;
    std::vector<std::uint32_t> page(w * h, 0xFF101010u);  // opaque page
    std::vector<std::uint32_t> chrome(w * h, 0u);
    // A hole at x 2..5, rows 1..2; one half-transparent caption pixel in it.
    for (int y = 1; y <= 2; y++) {
      for (int x = 2; x <= 5; x++) {
        page[y * w + x] = 0u;
      }
    }
    page[1 * w + 3] = 0x80404040u;  // premultiplied 50% grey
    std::vector<std::uint32_t> video(6 * 4, 0xFF0000FFu);  // blue (ARGB32)
    const display::Layers layers{page.data(), chrome.data(), w, h};
    // The video box covers x 1..6, rows 0..3, but only rows 1.. are content.
    const std::vector<display::VideoLayer> videos = {
        {video.data(), 6, Rect{1, 0, 6, 4}, Rect{0, 1, w, h - 1}}};
    std::vector<std::uint32_t> out(w * h, 0xDEADBEEFu);
    display::compose_rect(layers, {}, Rect{0, 0, w, h}, out.data(), w, videos);
    // Surface is ABGR: blue ends up in the low... swapped R/B: 0xFFFF0000.
    CHECK(out[1 * w + 2] == 0xFFFF0000u);  // hole: video
    CHECK(out[1 * w + 1] == display::swap_rb(0xFF101010u));  // page outside the hole
    CHECK(out[0 * w + 3] == display::swap_rb(0xFF101010u));  // above the clip: page
    // Caption over video: 0x40 grey + blue * 0.5.
    CHECK(out[1 * w + 3] == display::swap_rb(display::blend_over(0x80404040u, 0xFF0000FFu)));
    // Chrome over everything.
    std::vector<Rect> chrome_rects = {Rect{2, 2, 1, 1}};
    chrome[2 * w + 2] = 0xFF00FF00u;
    display::compose_rect(layers, chrome_rects, Rect{0, 0, w, h}, out.data(), w, videos);
    CHECK(out[2 * w + 2] == display::swap_rb(0xFF00FF00u));
    CHECK(out[2 * w + 3] == 0xFFFF0000u);
  }

  TEST_CASE("blend_row matches the scalar reference") {
    std::mt19937 rng(7);
    std::vector<std::uint32_t> over(67), under(67), a(67), b(67);
    for (std::size_t i = 0; i < over.size(); i++) {
      const std::uint32_t alpha = (i % 3 == 0) ? 0 : (i % 3 == 1) ? 255 : rng() & 0xFF;
      const std::uint32_t c = rng() & 0xFF;
      const std::uint32_t pm = c * alpha / 255;
      over[i] = (alpha << 24) | (pm << 16) | (pm << 8) | pm;
      under[i] = rng() | 0xFF000000u;
    }
    display::blend_row(over.data(), under.data(), a.data(), 67);
    display::blend_row_scalar(over.data(), under.data(), b.data(), 67);
    CHECK(a == b);
  }
}

TEST_SUITE("video_e2e") {
  void run_transport(wire::Transport transport) {
    std::atomic<int> updates{0};
    video::ServerOptions options;
    options.port = 0;
    options.stats_interval_ms = 0;
    options.on_update = [&] { updates++; };
    video::VideoServer server(options);
    REQUIRE(server.start());

    wire::VideoOverlayClient client;
    wire::VideoOverlayClient::Config config;
    config.port = server.port();
    config.pid = static_cast<std::uint32_t>(getpid());
    config.player = 1;
    config.force = transport;
    config.fileDirectory = "/tmp";
    std::vector<std::string> log;
    std::mutex log_lock;
    config.log = [&](const std::string& line) {
      std::lock_guard<std::mutex> lock(log_lock);
      log.push_back(line);
    };
    client.start(config);
    REQUIRE(wait_until([&] { return client.usable(); }));
    REQUIRE(wait_until([&] { return server.player_count() == 1; }));

    const video::PageKey page{config.pid, 77};
    client.setGeometry(wire::Geometry{77, 10, 20, 8, 4, true, false});
    // A 4x2 frame with padded rows (stride 6 pixels), scaled 2x to 8x4.
    std::vector<std::uint32_t> pixels(6 * 2, 0xFF00FF00u);
    pixels[0] = 0xFF0000FFu;
    std::atomic<int> released{0};
    wire::VideoOverlayClient::Frame frame;
    frame.pixels = reinterpret_cast<const std::uint8_t*>(pixels.data());
    frame.width = 4;
    frame.height = 2;
    frame.stride = 6 * 4;
    frame.pts = 1.0;
    frame.release = [&] { released++; };
    client.submit(std::move(frame));

    std::vector<std::uint32_t> got;
    REQUIRE(wait_until([&] {
      auto frames = server.acquire(page);
      if (frames.layers().size() != 1) {
        return false;
      }
      const auto& layer = frames.layers()[0];
      got.assign(layer.pixels, layer.pixels + layer.view_rect.area());
      return layer.view_rect == Rect{10, 20, 8, 4};
    }));
    CHECK(wait_until([&] { return released == 1; }));
    REQUIRE(got.size() == 32);
    CHECK(got[0] == 0xFF0000FFu);
    CHECK(got[31] == 0xFF00FF00u);
    CHECK(client.transport() == transport);
    auto damage = server.take_damage(page);
    CHECK_FALSE(damage.empty());
    CHECK(updates > 0);
    // Another page's frames are not handed out.
    CHECK(server.acquire(video::PageKey{config.pid, 78}).layers().empty());

    // Hidden: no layer, damage where it was.
    client.setGeometry(wire::Geometry{77, 10, 20, 8, 4, false, false});
    REQUIRE(wait_until([&] { return server.acquire(page).layers().empty(); }));
    // Visible again at another size: rescaled from the kept frame.
    client.setGeometry(wire::Geometry{77, 0, 0, 4, 2, true, false});
    REQUIRE(wait_until([&] {
      auto frames = server.acquire(page);
      return frames.layers().size() == 1 && frames.layers()[0].view_rect == Rect{0, 0, 4, 2};
    }));

    client.stop();
    REQUIRE(wait_until([&] { return server.player_count() == 0; }));
    server.stop();
    // The shared object's name was unlinked once both sides mapped it.
    if (transport == wire::Transport::File) {
      CHECK(access(("/tmp/video-" + std::to_string(config.pid) + "-1-1.shm").c_str(), F_OK) != 0);
    }
  }

  TEST_CASE("frames over a named shm object") { run_transport(wire::Transport::Shm); }
  TEST_CASE("frames over a shared file") { run_transport(wire::Transport::File); }
  TEST_CASE("frames inline over tcp") { run_transport(wire::Transport::Tcp); }

  TEST_CASE("no server: the client fails and releases its frame") {
    wire::VideoOverlayClient client;
    wire::VideoOverlayClient::Config config;
    config.port = 1;  // nothing listens on port 1
    config.connectTimeoutMs = 200;
    client.start(config);
    REQUIRE(wait_until([&] { return client.state() == wire::VideoOverlayClient::State::Failed; }));
    CHECK_FALSE(client.usable());
    std::atomic<int> released{0};
    wire::VideoOverlayClient::Frame frame;
    frame.release = [&] { released++; };
    client.submit(std::move(frame));
    client.stop();
    CHECK(released == 1);
  }
}
