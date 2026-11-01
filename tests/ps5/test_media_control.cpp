// SPDX-License-Identifier: GPL-3.0-or-later
//
// Media control-socket rendezvous (webkit-port/media/ffmpeg/
// FFmpegMediaControl.h): the UI cannot listen() under the Media
// application-category sandbox, so a WebProcess opens each audio/video stream
// by passing one end of a fresh socketpair to the UI over an inherited control
// socket. These tests cover the descriptor hand-off and framing, and an
// end-to-end run of the real client and server code over an adopted control
// socket (the same path as on the console, minus the sandbox).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <doctest/doctest.h>

#include "FFmpegMediaControl.h"
#include "FFmpegRemoteAudioClient.h"
#include "FFmpegVideoOverlayClient.h"
#include "audio_mixer.h"
#include "audio_server.h"
#include "video_server.h"

namespace mc = WebCore::FFmpegMedia::MediaControl;
namespace vwire = WebCore::FFmpegMedia::VideoWire;

using prosperoweb::audio::AudioServer;
using prosperoweb::audio::Mixer;

namespace {

template <typename Pred>
bool wait_until(Pred pred, int timeout_ms = 2000) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return pred();
}

bool readable(int fd, int timeout_ms) {
  pollfd p{fd, POLLIN, 0};
  return poll(&p, 1, timeout_ms) > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR));
}

}  // namespace

TEST_SUITE("media_control") {
  TEST_CASE("a descriptor passed over the control socket works on the other side") {
    int ctrl[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ctrl) == 0);

    // The "stream" the WebProcess hands over: one end of a throwaway pair.
    int stream[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, stream) == 0);
    REQUIRE(mc::sendDescriptor(ctrl[1], stream[1]));
    ::close(stream[1]);

    REQUIRE(readable(ctrl[0], 1000));
    int received = -1;
    REQUIRE(mc::receiveDescriptor(ctrl[0], received) == mc::Receive::Received);
    REQUIRE(received >= 0);

    // The received descriptor is a live duplicate: data sent on it reaches
    // stream[0], the end we kept.
    const char ping = 'Z';
    REQUIRE(::write(received, &ping, 1) == 1);
    char got = 0;
    REQUIRE(::read(stream[0], &got, 1) == 1);
    CHECK(got == 'Z');

    ::close(received);
    ::close(stream[0]);
    ::close(ctrl[0]);
    ::close(ctrl[1]);
  }

  TEST_CASE("multiple hand-offs over one control socket stay distinct") {
    int ctrl[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ctrl) == 0);
    int a[2];
    int b[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, a) == 0);
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, b) == 0);
    REQUIRE(mc::sendDescriptor(ctrl[1], a[1]));
    REQUIRE(mc::sendDescriptor(ctrl[1], b[1]));
    ::close(a[1]);
    ::close(b[1]);

    int ra = -1;
    int rb = -1;
    REQUIRE(readable(ctrl[0], 1000));
    REQUIRE(mc::receiveDescriptor(ctrl[0], ra) == mc::Receive::Received);
    REQUIRE(readable(ctrl[0], 1000));
    REQUIRE(mc::receiveDescriptor(ctrl[0], rb) == mc::Receive::Received);

    const char x = 'a';
    const char y = 'b';
    REQUIRE(::write(ra, &x, 1) == 1);
    REQUIRE(::write(rb, &y, 1) == 1);
    char ga = 0;
    char gb = 0;
    REQUIRE(::read(a[0], &ga, 1) == 1);
    REQUIRE(::read(b[0], &gb, 1) == 1);
    CHECK(ga == 'a');
    CHECK(gb == 'b');

    for (int fd : {ra, rb, a[0], b[0], ctrl[0], ctrl[1]}) {
      ::close(fd);
    }
  }

  TEST_CASE("a closed WebProcess end reports Closed") {
    int ctrl[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ctrl) == 0);
    ::close(ctrl[1]);
    REQUIRE(readable(ctrl[0], 1000));
    int fd = -1;
    CHECK(mc::receiveDescriptor(ctrl[0], fd) == mc::Receive::Closed);
    CHECK(fd == -1);
    ::close(ctrl[0]);
  }

  TEST_CASE("controlFdFromEnv parses only a descriptor number") {
    setenv("PROSPEROWEB_TEST_CTRL_FD", "7", 1);
    CHECK(mc::controlFdFromEnv("PROSPEROWEB_TEST_CTRL_FD") == 7);
    setenv("PROSPEROWEB_TEST_CTRL_FD", "", 1);
    CHECK(mc::controlFdFromEnv("PROSPEROWEB_TEST_CTRL_FD") == -1);
    setenv("PROSPEROWEB_TEST_CTRL_FD", "x12", 1);
    CHECK(mc::controlFdFromEnv("PROSPEROWEB_TEST_CTRL_FD") == -1);
    unsetenv("PROSPEROWEB_TEST_CTRL_FD");
    CHECK(mc::controlFdFromEnv("PROSPEROWEB_TEST_CTRL_FD") == -1);
  }
}

TEST_SUITE("media_audio_control") {
  TEST_CASE("audio streams end to end over an adopted control socket") {
    Mixer mixer(48000);
    prosperoweb::audio::ServerOptions options;
    options.port = 0;
    options.output_latency_frames = 2048;
    options.listen_optional = true;
    AudioServer server(mixer, options);
    REQUIRE(server.start());

    // The control socket a WebProcess would inherit: one end to the server.
    int ctrl[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ctrl) == 0);
    server.add_control_fd(ctrl[0]);

    std::atomic<bool> stop_output{false};
    std::atomic<std::uint64_t> output_frames{0};
    std::thread output([&] {
      std::vector<float> buf(480 * 2);
      while (!stop_output) {
        mixer.mix(buf.data(), 480);
        output_frames += 480;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });

    WebCore::FFmpegMedia::AudioWire::RemoteAudioClient client;
    client.setTargetAhead(0.1);
    std::string error;
    REQUIRE(client.connectControl(ctrl[1], 2000, error));
    CHECK(client.accepted().outputLatencyFrames == 2048);
    REQUIRE(wait_until([&] { return server.client_count() == 1; }));

    constexpr std::uint64_t total = 9600;  // 0.2 s
    std::uint64_t produced = 0;
    auto pull = [&](float* out, std::size_t frames) {
      for (std::size_t i = 0; i < frames * 2; ++i) {
        out[i] = produced + i / 2 < total ? 0.4f : 0.0f;
      }
      produced += frames;
    };
    client.setPlaying(true);
    auto start = std::chrono::steady_clock::now();
    while (produced < total &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
      REQUIRE(client.pump(pull));
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(produced >= total);
    CHECK(wait_until([&] {
      client.pump(pull);
      return client.estimatedRetired() >= total;
    }, 3000));

    client.requestClose();
    client.pump(pull);
    CHECK(wait_until([&] { return server.client_count() == 0; }));
    stop_output = true;
    output.join();
    server.stop();
    ::close(ctrl[1]);
  }

  TEST_CASE("the TCP path still works when a control fd is not used") {
    Mixer mixer(48000);
    prosperoweb::audio::ServerOptions options;
    options.port = 0;
    options.listen_optional = true;
    AudioServer server(mixer, options);
    REQUIRE(server.start());
    REQUIRE(server.listening());  // on the host listen() always succeeds
    WebCore::FFmpegMedia::AudioWire::RemoteAudioClient client;
    std::string error;
    REQUIRE(client.connect(server.port(), 1000, error));
    CHECK(wait_until([&] { return server.client_count() == 1; }));
    server.stop();
  }
}

TEST_SUITE("media_video_control") {
  TEST_CASE("a player connects and delivers a frame over an adopted control socket") {
    prosperoweb::video::ServerOptions options;
    options.port = 0;
    options.stats_interval_ms = 0;
    prosperoweb::video::VideoServer server(options);
    REQUIRE(server.start());

    int ctrl[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, ctrl) == 0);
    server.add_control_fd(ctrl[0]);

    vwire::VideoOverlayClient client;
    vwire::VideoOverlayClient::Config config;
    config.controlFd = ctrl[1];
    config.pid = static_cast<std::uint32_t>(getpid());
    config.player = 1;
    config.force = vwire::Transport::Tcp;  // pixels inline, no shm files
    client.start(config);
    REQUIRE(wait_until([&] { return client.usable(); }));
    REQUIRE(wait_until([&] { return server.player_count() == 1; }));

    const prosperoweb::video::PageKey page{config.pid, 77};
    client.setGeometry(vwire::Geometry{77, 10, 20, 8, 4, true, false});
    std::vector<std::uint32_t> pixels(6 * 2, 0xFF00FF00u);
    pixels[0] = 0xFF0000FFu;
    std::atomic<int> released{0};
    vwire::VideoOverlayClient::Frame frame;
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
      return layer.view_rect == prosperoweb::display::Rect{10, 20, 8, 4};
    }));
    CHECK(wait_until([&] { return released == 1; }));
    REQUIRE(got.size() == 32);
    CHECK(got[0] == 0xFF0000FFu);
    CHECK(client.transport() == vwire::Transport::Tcp);

    client.stop();
    REQUIRE(wait_until([&] { return server.player_count() == 0; }));
    server.stop();
    ::close(ctrl[1]);
  }
}
