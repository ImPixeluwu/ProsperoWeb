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

namespace prosperoweb::audio {

struct ServerOptions {
  int port = 17772;                       // 0 = any free port (tests)
  std::uint32_t output_latency_frames = 0;  // reported to clients
  int report_interval_ms = 20;
  // When true, start() keeps running even if listen() is refused (the Media
  // application-category sandbox forbids it, console run 34): the server then
  // serves only clients adopted through add_control_fd(). False keeps the
  // old behaviour, where listen() must succeed.
  bool listen_optional = false;
};

class AudioServer {
 public:
  AudioServer(Mixer& mixer, ServerOptions options);
  ~AudioServer();
  AudioServer(const AudioServer&) = delete;
  AudioServer& operator=(const AudioServer&) = delete;

  // Binds 127.0.0.1:port and starts the thread. False if it cannot listen,
  // unless ServerOptions::listen_optional is set (then the thread still
  // starts and serves only control-socket clients).
  bool start();
  void stop();
  int port() const { return port_; }
  // Whether listen() succeeded (false under the media-category sandbox).
  bool listening() const { return listening_; }
  std::size_t client_count() const { return clients_.load(); }

  // Adopts an inherited AF_UNIX control socket (one per WebProcess): the
  // server polls it and, for every stream descriptor the WebProcess passes
  // over it (FFmpegMediaControl.h), takes on a new client exactly as an
  // accept() would. Thread-safe; takes ownership of `fd`.
  void add_control_fd(int fd);

  // Queues interleaved stereo samples as a local stream (no client); once it
  // has played out it is removed and `done_message` is logged. Samples beyond
  // the mixer's capacity are dropped.
  void play_local(const std::vector<float>& samples, std::string done_message);

 private:
  struct Connection;
  void run();
  void accept_clients();
  void service_control_fds();
  // Sets up `fd` as a new client connection (non-blocking, NODELAY) and adds
  // it to connections_. Server thread only. Used by accept and control.
  void adopt_fd(int fd);
  bool service(Connection& c);
  bool handle_message(Connection& c);
  void send_reports();
  void close_connection(Connection& c, const char* why);
  void check_local_streams();

  Mixer& mixer_;
  ServerOptions options_;
  int listen_fd_ = -1;
  bool listening_ = false;
  bool logged_socket_mode_ = false;  // the client socket mode is logged once
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<std::size_t> clients_{0};
  std::thread thread_;
  std::vector<std::unique_ptr<Connection>> connections_;  // server thread only
  std::vector<int> control_fds_;  // server thread only: active control sockets
  std::mutex control_mu_;
  std::vector<int> pending_control_fds_;  // control_mu_: handed in by add_control_fd

  struct LocalStream {
    std::uint32_t id;
    std::string done_message;
  };
  std::mutex local_lock_;
  std::vector<LocalStream> local_streams_;
};

}  // namespace prosperoweb::audio
