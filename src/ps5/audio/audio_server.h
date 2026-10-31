// SPDX-License-Identifier: GPL-3.0-or-later
//
// Audio server of the UI process. On the PS5 only the foreground
// application's audio is heard, so the WebProcess streams its decoded PCM
// here (protocol: webkit-port/media/ffmpeg/FFmpegAudioWire.h) and the UI
// mixes every stream into its own output device through Mixer.
//
// One thread polls the listening socket and all clients: it parses frames
// into the mixer and sends each client a played report every
// report_interval_ms. It never blocks on a client: reads are non-blocking
// and a report that does not fit into the socket buffer is skipped.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audio_mixer.h"

namespace prosperobrowser::audio {

struct ServerOptions {
  int port = 17772;                       // 0 = any free port (tests)
  std::uint32_t output_latency_frames = 0;  // reported to clients
  int report_interval_ms = 20;
};

class AudioServer {
 public:
  AudioServer(Mixer& mixer, ServerOptions options);
  ~AudioServer();
  AudioServer(const AudioServer&) = delete;
  AudioServer& operator=(const AudioServer&) = delete;

  // Binds 127.0.0.1:port and starts the thread. False if it cannot listen.
  bool start();
  void stop();
  int port() const { return port_; }
  std::size_t client_count() const { return clients_.load(); }

  // Queues interleaved stereo samples as a local stream (no client); once it
  // has played out it is removed and `done_message` is logged. Samples beyond
  // the mixer's capacity are dropped.
  void play_local(const std::vector<float>& samples, std::string done_message);

 private:
  struct Connection;
  void run();
  void accept_clients();
  bool service(Connection& c);
  bool handle_message(Connection& c);
  void send_reports();
  void close_connection(Connection& c, const char* why);
  void check_local_streams();

  Mixer& mixer_;
  ServerOptions options_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<std::size_t> clients_{0};
  std::thread thread_;
  std::vector<std::unique_ptr<Connection>> connections_;  // server thread only

  struct LocalStream {
    std::uint32_t id;
    std::string done_message;
  };
  std::mutex local_lock_;
  std::vector<LocalStream> local_streams_;
};

}  // namespace prosperobrowser::audio
