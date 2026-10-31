// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audio forwarding from the WebProcess to the UI process: wire format
// (webkit-port/media/ffmpeg/FFmpegAudioWire.h), the client clock, the UI
// mixer and an end-to-end loopback through AudioServer and
// RemoteAudioClient (the code RemoteAudioSink runs in the WebProcess).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "FFmpegAudioWire.h"
#include "FFmpegRemoteAudioClient.h"
#include "audio_mixer.h"
#include "audio_server.h"

using namespace WebCore::FFmpegMedia::AudioWire;
using prosperobrowser::audio::AudioServer;
using prosperobrowser::audio::Mixer;
using prosperobrowser::audio::ServerOptions;

namespace {

std::vector<float> constant_frames(std::size_t frames, float left, float right) {
  std::vector<float> v(frames * 2);
  for (std::size_t i = 0; i < frames; ++i) {
    v[2 * i] = left;
    v[2 * i + 1] = right;
  }
  return v;
}

template <typename Pred>
bool wait_until(Pred pred, int timeout_ms) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return pred();
}

}  // namespace

TEST_SUITE("audio_wire") {
  TEST_CASE("hello round-trip and validation") {
    std::uint8_t buf[helloSize];
    encodeHello(Hello{}, buf);
    CHECK(std::memcmp(buf, "PS5A", 4) == 0);
    auto hello = decodeHello(buf);
    REQUIRE(hello);
    CHECK(hello->version == 1);
    CHECK(hello->rate == 48000);
    CHECK(hello->channels == 2);
    CHECK(hello->format == formatF32);
    CHECK(helloSupported(*hello));
    Hello mono;
    mono.channels = 1;
    encodeHello(mono, buf);
    CHECK_FALSE(helloSupported(*decodeHello(buf)));
    buf[0] = 'X';
    CHECK_FALSE(decodeHello(buf));
  }

  TEST_CASE("integers are little-endian") {
    std::uint8_t buf[8];
    putU32(buf, 0x11223344u);
    CHECK(buf[0] == 0x44);
    CHECK(buf[3] == 0x11);
    putU64(buf, 0x0102030405060708ull);
    CHECK(getU64(buf) == 0x0102030405060708ull);
    putU16(buf, 0xBEEF);
    CHECK(getU16(buf) == 0xBEEF);
  }

  TEST_CASE("messages survive byte-by-byte delivery") {
    std::vector<std::uint8_t> stream(helloSize);
    encodeHello(Hello{}, stream.data());
    auto pcm = constant_frames(3, 0.25f, -0.5f);
    appendMessage(stream, Type::Pcm, pcm.data(), pcm.size() * sizeof(float));
    appendMessage(stream, Type::Flush);
    appendPause(stream, true);
    appendVolume(stream, 0.5f);
    appendPlayed(stream, {123456789012ull, 4800});
    appendAccepted(stream, {7, 2048});
    appendMessage(stream, Type::Close);

    FrameParser parser(true);
    std::vector<FrameParser::Result> results;
    std::vector<Type> types;
    float first_sample = 0;
    Played played;
    Accepted accepted;
    float gain = 0;
    bool paused = false;
    for (std::uint8_t byte : stream) {
      parser.append(&byte, 1);
      for (auto r = parser.next(); r != FrameParser::Result::NeedMore; r = parser.next()) {
        results.push_back(r);
        if (r != FrameParser::Result::Message) {
          continue;
        }
        const auto& m = parser.message();
        types.push_back(m.type);
        if (m.type == Type::Pcm) {
          CHECK(m.bytes == 3 * 2 * sizeof(float));
          std::memcpy(&first_sample, m.payload + sizeof(float), sizeof(float));
        } else if (m.type == Type::Played) {
          played = *decodePlayed(m.payload, m.bytes);
        } else if (m.type == Type::Accepted) {
          accepted = *decodeAccepted(m.payload, m.bytes);
        } else if (m.type == Type::Volume) {
          gain = *decodeVolume(m.payload, m.bytes);
        } else if (m.type == Type::Pause) {
          paused = m.bytes == 1 && m.payload[0] == 1;
        }
      }
    }
    REQUIRE(results.size() == 8);
    CHECK(results[0] == FrameParser::Result::Hello);
    CHECK(types == std::vector<Type>{Type::Pcm, Type::Flush, Type::Pause, Type::Volume,
                                     Type::Played, Type::Accepted, Type::Close});
    CHECK(first_sample == -0.5f);
    CHECK(played.framesRetired == 123456789012ull);
    CHECK(played.queuedFrames == 4800);
    CHECK(accepted.streamId == 7);
    CHECK(accepted.outputLatencyFrames == 2048);
    CHECK(gain == 0.5f);
    CHECK(paused);
    CHECK(parser.pendingBytes() == 0);
  }

  TEST_CASE("bad magic and oversized payloads are errors") {
    FrameParser hello_parser(true);
    const std::uint8_t junk[helloSize] = {'H', 'T', 'T', 'P'};
    hello_parser.append(junk, sizeof(junk));
    CHECK(hello_parser.next() == FrameParser::Result::Error);

    FrameParser parser;
    std::uint8_t header[headerSize];
    encodeHeader(Type::Pcm, maxPayload + 1, header);
    parser.append(header, sizeof(header));
    CHECK(parser.next() == FrameParser::Result::Error);
  }

  TEST_CASE("volume is clamped and malformed payloads rejected") {
    float big = 3.0f;
    CHECK(*decodeVolume(reinterpret_cast<std::uint8_t*>(&big), sizeof(big)) == 1.0f);
    float nan = std::nanf("");
    CHECK(*decodeVolume(reinterpret_cast<std::uint8_t*>(&nan), sizeof(nan)) == 0.0f);
    std::uint8_t short_payload[3] = {};
    CHECK_FALSE(decodeVolume(short_payload, 3));
    CHECK_FALSE(decodePlayed(short_payload, 3));
    CHECK_FALSE(decodeAccepted(short_payload, 3));
  }
}

TEST_SUITE("audio_clock") {
  TEST_CASE("reports map to latency") {
    RemoteAudioClock clock(48000);
    clock.setOutputLatencyFrames(2048);
    clock.setPlaying(true, 0.0);
    clock.sent(4800);
    CHECK(clock.aheadFrames(0.0) == 4800);  // no report yet: nothing played
    clock.report({960, 3840}, 1.0);
    CHECK(clock.aheadFrames(1.0) == 3840);
    CHECK(clock.latencySeconds(1.0) == doctest::Approx((3840.0 + 2048) / 48000));
    // 10 ms later the position extrapolates by 480 frames.
    CHECK(clock.aheadFrames(1.010) == 3360);
  }

  TEST_CASE("extrapolation is capped and never passes sent") {
    RemoteAudioClock clock(48000);
    clock.setPlaying(true, 0.0);
    clock.sent(48000);
    clock.report({0, 48000}, 0.0);
    // A server that stops reporting stalls the clock after 100 ms.
    CHECK(clock.estimatedRetired(5.0) == 4800);
    RemoteAudioClock small(48000);
    small.setPlaying(true, 0.0);
    small.sent(100);
    small.report({0, 100}, 0.0);
    CHECK(small.estimatedRetired(0.05) == 100);
  }

  TEST_CASE("never goes backwards, freezes while paused") {
    RemoteAudioClock clock(48000);
    clock.setPlaying(true, 0.0);
    clock.sent(9600);
    clock.report({1000, 0}, 0.0);
    auto a = clock.estimatedRetired(0.02);  // 1000 + 960
    CHECK(a == 1960);
    clock.report({1500, 0}, 0.021);  // the server was behind the estimate
    CHECK(clock.estimatedRetired(0.021) >= a);
    clock.setPlaying(false, 0.03);
    auto frozen = clock.estimatedRetired(0.03);
    CHECK(clock.estimatedRetired(1.0) == frozen);
    clock.setPlaying(true, 2.0);
    CHECK(clock.estimatedRetired(2.0) == frozen);
    CHECK(clock.estimatedRetired(2.01) == frozen + 480);
  }

  TEST_CASE("flush retires everything sent") {
    RemoteAudioClock clock(48000);
    clock.setPlaying(true, 0.0);
    clock.sent(4800);
    clock.report({480, 4320}, 0.0);
    clock.retireAllSent();
    CHECK(clock.aheadFrames(0.0) == 0);
    clock.sent(960);
    CHECK(clock.aheadFrames(0.0) == 960);
    // A stale report from before the flush does not move it back.
    clock.report({600, 4200}, 0.001);
    CHECK(clock.aheadFrames(0.001) <= 960);
  }
}

TEST_SUITE("audio_mixer") {
  TEST_CASE("two streams are summed with per-stream gain and clipped") {
    Mixer mixer(1000);
    auto a = mixer.add_stream();
    auto b = mixer.add_stream();
    CHECK(a != b);
    CHECK(mixer.stream_count() == 2);
    auto loud = constant_frames(4, 0.75f, -0.75f);
    auto quiet = constant_frames(4, 0.5f, 0.5f);
    CHECK(mixer.write(a, loud.data(), 4) == 4);
    CHECK(mixer.write(b, quiet.data(), 4) == 4);
    mixer.set_volume(b, 0.5f);
    std::vector<float> out(4 * 2);
    mixer.mix(out.data(), 2);
    CHECK(out[0] == doctest::Approx(1.0f));    // 0.75 + 0.25
    CHECK(out[1] == doctest::Approx(-0.5f));   // -0.75 + 0.25
    mixer.set_volume(b, 1.0f);
    mixer.mix(out.data(), 2);
    CHECK(out[0] == 1.0f);                     // 1.25 clipped
    CHECK(out[1] == doctest::Approx(-0.25f));
    auto hot = constant_frames(1, -0.9f, -0.9f);
    mixer.write(a, hot.data(), 1);
    mixer.write(b, hot.data(), 1);
    mixer.mix(out.data(), 1);
    CHECK(out[0] == -1.0f);                    // -1.8 clipped
  }

  TEST_CASE("paused streams keep their data and do not advance") {
    Mixer mixer(100);
    auto id = mixer.add_stream();
    auto data = constant_frames(10, 0.1f, 0.1f);
    mixer.write(id, data.data(), 10);
    mixer.set_paused(id, true);
    std::vector<float> out(20);
    mixer.mix(out.data(), 10);
    CHECK(out[0] == 0.0f);
    Mixer::Status s;
    REQUIRE(mixer.status(id, s));
    CHECK(s.queued == 10);
    CHECK(s.retired == 0);
    CHECK(s.paused);
    mixer.set_paused(id, false);
    mixer.mix(out.data(), 10);
    REQUIRE(mixer.status(id, s));
    CHECK(s.retired == 10);
    CHECK(s.queued == 0);
  }
}

TEST_SUITE("audio_ring") {
  TEST_CASE("underrun is counted once per dry spell and played silence is not retired") {
    Mixer mixer(100);
    auto id = mixer.add_stream();
    std::vector<float> out(64 * 2);
    mixer.mix(out.data(), 64);  // never had data: not an underrun
    Mixer::Status s;
    REQUIRE(mixer.status(id, s));
    CHECK(s.underruns == 0);
    auto data = constant_frames(10, 0.2f, 0.2f);
    mixer.write(id, data.data(), 10);
    mixer.mix(out.data(), 64);
    CHECK(out[19] == doctest::Approx(0.2f));
    CHECK(out[20] == 0.0f);
    mixer.mix(out.data(), 64);
    REQUIRE(mixer.status(id, s));
    CHECK(s.underruns == 1);
    CHECK(s.retired == 10);
  }

  TEST_CASE("overrun drops the excess and counts it as retired") {
    Mixer mixer(100);
    auto id = mixer.add_stream();
    auto data = constant_frames(150, 0.3f, 0.3f);
    CHECK(mixer.write(id, data.data(), 150) == 100);
    Mixer::Status s;
    REQUIRE(mixer.status(id, s));
    CHECK(s.queued == 100);
    CHECK(s.overflowed == 50);
    CHECK(s.retired == 50);
  }

  TEST_CASE("flush retires queued frames") {
    Mixer mixer(100);
    auto id = mixer.add_stream();
    auto data = constant_frames(40, 0.3f, 0.3f);
    mixer.write(id, data.data(), 40);
    mixer.flush(id);
    Mixer::Status s;
    REQUIRE(mixer.status(id, s));
    CHECK(s.queued == 0);
    CHECK(s.retired == 40);
    std::vector<float> out(8);
    mixer.mix(out.data(), 4);
    REQUIRE(mixer.status(id, s));
    CHECK(s.underruns == 0);  // empty after a flush is not an underrun
    mixer.remove_stream(id);
    CHECK_FALSE(mixer.status(id, s));
  }
}

TEST_SUITE("audio_e2e") {
  TEST_CASE("client streams 0.5 s through the server and its clock follows playback") {
    Mixer mixer(48000);
    ServerOptions options;
    options.port = 0;
    options.output_latency_frames = 2048;
    AudioServer server(mixer, options);
    REQUIRE(server.start());

    // Fake output device: 10 ms per tick in real time.
    std::atomic<bool> stop_output{false};
    std::atomic<std::uint64_t> output_frames{0};
    std::atomic<float> peak{0};
    std::thread output([&] {
      std::vector<float> buf(480 * 2);
      while (!stop_output) {
        mixer.mix(buf.data(), 480);
        float p = peak;
        for (float v : buf) {
          p = std::max(p, std::fabs(v));
        }
        peak = p;
        output_frames += 480;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });

    RemoteAudioClient client;
    client.setTargetAhead(0.1);
    std::string error;
    REQUIRE(client.connect(server.port(), 1000, error));
    CHECK(client.accepted().outputLatencyFrames == 2048);
    CHECK(client.accepted().streamId >= 1);
    CHECK(wait_until([&] { return server.client_count() == 1; }, 1000));

    constexpr std::uint64_t total = 24000;  // 0.5 s
    std::uint64_t produced = 0;
    auto pull = [&](float* out, std::size_t frames) {
      for (std::size_t i = 0; i < frames * 2; ++i) {
        out[i] = produced + i / 2 < total ? 0.5f : 0.0f;
      }
      produced += frames;
    };
    client.setPlaying(true);
    std::uint64_t first_retired = 0;
    bool bounded = true;
    auto start = std::chrono::steady_clock::now();
    while (produced < total &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(5)) {
      REQUIRE(client.pump(pull));
      // Never more than the target ahead of what the server retired.
      bounded = bounded && client.totalSent() - client.estimatedRetired() <= 4800 + 480;
      if (!first_retired) {
        first_retired = client.estimatedRetired();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(produced >= total);
    CHECK(bounded);
    // The server reports what it played; the client's estimate follows.
    CHECK(wait_until([&] {
      client.pump(pull);
      return client.estimatedRetired() >= total;
    }, 3000));
    CHECK(client.estimatedRetired() > first_retired);
    CHECK(client.latencySeconds() >= 2048.0 / 48000 - 1e-9);
    CHECK(peak.load() == doctest::Approx(0.5f));

    // Pause: the server stops consuming, the clock freezes.
    client.setPlaying(false);
    client.pump(pull);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    client.pump(pull);
    const auto paused_at = client.estimatedRetired();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    client.pump(pull);
    CHECK(client.estimatedRetired() == paused_at);

    // Flush (seek): nothing is ahead any more.
    client.requestFlush();
    client.pump(pull);
    CHECK(client.totalSent() - client.estimatedRetired() == 0);

    client.requestClose();
    client.pump(pull);
    CHECK(wait_until([&] { return server.client_count() == 0 && mixer.stream_count() == 0; },
                     1000));
    stop_output = true;
    output.join();
    server.stop();
  }

  TEST_CASE("a lost server falls back to local pacing") {
    Mixer mixer;
    ServerOptions options;
    options.port = 0;
    auto server = std::make_unique<AudioServer>(mixer, options);
    REQUIRE(server->start());
    RemoteAudioClient client;
    std::string error;
    REQUIRE(client.connect(server->port(), 1000, error));
    server.reset();
    std::uint64_t pulled = 0;
    auto pull = [&](float*, std::size_t frames) { pulled += frames; };
    client.setPlaying(true);
    bool failed = wait_until([&] { return !client.pump(pull); }, 2000);
    CHECK(failed);
    CHECK(client.failed());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    client.pump(pull);
    const auto before = pulled;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    client.pump(pull);
    CHECK(pulled > before);  // still consuming in real time
    CHECK(client.latencySeconds() == 0.0);
  }

  TEST_CASE("connection refused and unsupported hello are reported") {
    Mixer mixer;
    ServerOptions options;
    options.port = 0;
    int port = 0;
    {
      AudioServer probe(mixer, options);
      REQUIRE(probe.start());
      port = probe.port();
    }
    RemoteAudioClient client;
    std::string error;
    CHECK_FALSE(client.connect(port, 300, error));
    CHECK_FALSE(error.empty());

    AudioServer server(mixer, options);
    REQUIRE(server.start());
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(server.port()));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    Hello mono;
    mono.channels = 1;
    std::uint8_t hello[helloSize];
    encodeHello(mono, hello);
    REQUIRE(send(fd, hello, sizeof(hello), 0) == static_cast<ssize_t>(sizeof(hello)));
    std::uint8_t reply[headerSize];
    REQUIRE(recv(fd, reply, sizeof(reply), MSG_WAITALL) == static_cast<ssize_t>(sizeof(reply)));
    CHECK(reply[0] == static_cast<std::uint8_t>(Type::Rejected));
    std::uint8_t more;
    CHECK(recv(fd, &more, 1, 0) == 0);  // closed by the server
    close(fd);
    CHECK(mixer.stream_count() == 0);
  }

  TEST_CASE("local streams play out and are removed") {
    Mixer mixer;
    ServerOptions options;
    options.port = 0;
    AudioServer server(mixer, options);
    REQUIRE(server.start());
    server.play_local(constant_frames(960, 0.1f, 0.1f), "test tone done");
    CHECK(mixer.stream_count() == 1);
    std::vector<float> out(480 * 2);
    mixer.mix(out.data(), 480);
    mixer.mix(out.data(), 480);
    CHECK(wait_until([&] { return mixer.stream_count() == 0; }, 1000));
  }
}
