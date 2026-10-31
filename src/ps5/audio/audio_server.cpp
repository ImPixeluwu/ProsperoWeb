// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio_server.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "FFmpegAudioWire.h"

extern "C" void ps5_log_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

namespace prosperobrowser::audio {

namespace wire = WebCore::FFmpegMedia::AudioWire;

namespace {

using Clock = std::chrono::steady_clock;

int send_flags() {
#ifdef MSG_NOSIGNAL
  return MSG_NOSIGNAL;
#else
  return 0;
#endif
}

void set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

}  // namespace

struct AudioServer::Connection {
  int fd = -1;
  std::uint32_t stream = 0;  // 0 until the hello was accepted
  wire::FrameParser parser{/*expectHello=*/true};
  std::vector<float> samples;  // aligned copy of a pcm payload
  std::vector<std::uint8_t> out;
  std::uint32_t underruns_logged = 0;
  bool closed = false;
};

AudioServer::AudioServer(Mixer& mixer, ServerOptions options)
    : mixer_(mixer), options_(options) {}

AudioServer::~AudioServer() { stop(); }

bool AudioServer::start() {
  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    ps5_log_printf("audio server: socket: %s", std::strerror(errno));
    return false;
  }
  int one = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(static_cast<std::uint16_t>(options_.port));
  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      listen(listen_fd_, 8) != 0) {
    ps5_log_printf("audio server: cannot listen on tcp 127.0.0.1:%d: %s", options_.port,
                   std::strerror(errno));
    close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  socklen_t len = sizeof(addr);
  getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  port_ = ntohs(addr.sin_port);
  set_nonblocking(listen_fd_);
  ps5_log_printf("audio server: listening on tcp 127.0.0.1:%d", port_);
  stop_ = false;
  thread_ = std::thread([this] { run(); });
  return true;
}

void AudioServer::stop() {
  stop_ = true;
  if (thread_.joinable()) {
    thread_.join();
  }
  for (auto& c : connections_) {
    close_connection(*c, "server stopped");
  }
  connections_.clear();
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    listen_fd_ = -1;
  }
}

void AudioServer::play_local(const std::vector<float>& samples, std::string done_message) {
  const std::uint32_t id = mixer_.add_stream();
  mixer_.write(id, samples.data(), samples.size() / Mixer::kChannels);
  std::lock_guard<std::mutex> lock(local_lock_);
  local_streams_.push_back({id, std::move(done_message)});
}

void AudioServer::run() {
  const auto interval = std::chrono::milliseconds(options_.report_interval_ms);
  auto next_report = Clock::now() + interval;
  std::vector<pollfd> fds;
  while (!stop_) {
    fds.clear();
    fds.push_back({listen_fd_, POLLIN, 0});
    for (auto& c : connections_) {
      fds.push_back({c->fd, POLLIN, 0});
    }
    const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_report - Clock::now());
    const int timeout = static_cast<int>(std::clamp<std::int64_t>(wait.count(), 0, 20));
    const int ready = poll(fds.data(), fds.size(), timeout);
    if (ready < 0 && errno != EINTR) {
      ps5_log_printf("audio server: poll: %s", std::strerror(errno));
      return;
    }
    if (ready > 0) {
      // connections_ may grow in accept_clients(); fds covers the old ones.
      for (std::size_t i = 1; i < fds.size(); ++i) {
        if (fds[i].revents) {
          Connection& c = *connections_[i - 1];
          if (!service(c)) {
            close_connection(c, nullptr);
          }
        }
      }
      if (fds[0].revents & POLLIN) {
        accept_clients();
      }
      connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                                        [](const auto& c) { return c->closed; }),
                         connections_.end());
      clients_ = connections_.size();
    }
    if (Clock::now() >= next_report) {
      next_report = Clock::now() + interval;
      send_reports();
      check_local_streams();
    }
  }
}

void AudioServer::accept_clients() {
  while (true) {
    const int fd = accept(listen_fd_, nullptr, nullptr);
    if (fd < 0) {
      return;
    }
    set_nonblocking(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    auto c = std::make_unique<Connection>();
    c->fd = fd;
    connections_.push_back(std::move(c));
  }
}

bool AudioServer::service(Connection& c) {
  std::uint8_t buffer[16384];
  while (true) {
    const ssize_t got = recv(c.fd, buffer, sizeof(buffer), 0);
    if (got > 0) {
      c.parser.append(buffer, static_cast<std::size_t>(got));
      // Parse as we go so the parser's buffer stays small.
      while (true) {
        const auto result = c.parser.next();
        if (result == wire::FrameParser::Result::NeedMore) {
          break;
        }
        if (result == wire::FrameParser::Result::Error) {
          ps5_log_printf("audio server: stream %u: protocol error", c.stream);
          return false;
        }
        if (result == wire::FrameParser::Result::Hello) {
          const auto& hello = c.parser.hello();
          if (!wire::helloSupported(hello)) {
            ps5_log_printf("audio server: rejected a client: version %u, %u Hz, %u ch, format %u",
                           hello.version, hello.rate, hello.channels, hello.format);
            c.out.clear();
            wire::appendMessage(c.out, wire::Type::Rejected);
            send(c.fd, c.out.data(), c.out.size(), send_flags());
            return false;
          }
          c.stream = mixer_.add_stream();
          c.out.clear();
          wire::appendAccepted(c.out, {c.stream, options_.output_latency_frames});
          send(c.fd, c.out.data(), c.out.size(), send_flags());
          ps5_log_printf("audio server: stream %u connected (%u Hz %u ch)", c.stream, hello.rate,
                         hello.channels);
          continue;
        }
        if (!handle_message(c)) {
          return false;
        }
      }
      continue;
    }
    if (got == 0) {
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK;
  }
}

bool AudioServer::handle_message(Connection& c) {
  const auto& m = c.parser.message();
  switch (m.type) {
    case wire::Type::Pcm: {
      constexpr std::size_t frame_bytes = sizeof(float) * Mixer::kChannels;
      if (m.bytes % frame_bytes) {
        ps5_log_printf("audio server: stream %u: pcm payload of %u bytes is not whole frames",
                       c.stream, m.bytes);
        return false;
      }
      c.samples.resize(m.bytes / sizeof(float));
      std::memcpy(c.samples.data(), m.payload, m.bytes);
      const std::size_t frames = m.bytes / frame_bytes;
      if (mixer_.write(c.stream, c.samples.data(), frames) < frames) {
        Mixer::Status s;
        if (mixer_.status(c.stream, s) && s.overflowed == frames) {
          ps5_log_printf("audio server: stream %u: buffer full, dropping samples", c.stream);
        }
      }
      return true;
    }
    case wire::Type::Flush:
      mixer_.flush(c.stream);
      return true;
    case wire::Type::Pause:
      if (m.bytes == 1) {
        mixer_.set_paused(c.stream, m.payload[0] != 0);
      }
      return true;
    case wire::Type::Volume:
      if (auto gain = wire::decodeVolume(m.payload, m.bytes)) {
        mixer_.set_volume(c.stream, *gain);
      }
      return true;
    case wire::Type::Close:
      return false;
    default:
      return true;  // unknown types are skipped (forward compatibility)
  }
}

void AudioServer::send_reports() {
  for (auto& c : connections_) {
    if (!c->stream) {
      continue;
    }
    Mixer::Status s;
    if (!mixer_.status(c->stream, s)) {
      continue;
    }
    if (s.underruns && !c->underruns_logged) {
      ps5_log_printf("audio server: stream %u: underrun (the client is not keeping up; "
                     "logged once per stream)", c->stream);
    }
    c->underruns_logged = s.underruns;
    c->out.clear();
    wire::appendPlayed(c->out, {s.retired, s.queued});
    // Non-blocking: when the client does not read, skip this report.
    send(c->fd, c->out.data(), c->out.size(), send_flags() | MSG_DONTWAIT);
  }
}

void AudioServer::close_connection(Connection& c, const char* why) {
  if (c.closed) {
    return;
  }
  c.closed = true;
  if (c.stream) {
    Mixer::Status s;
    mixer_.status(c.stream, s);
    ps5_log_printf("audio server: stream %u closed%s%s (%llu frames, %u underruns, %llu dropped)",
                   c.stream, why ? ": " : "", why ? why : "",
                   static_cast<unsigned long long>(s.retired), s.underruns,
                   static_cast<unsigned long long>(s.overflowed));
    mixer_.remove_stream(c.stream);
  }
  close(c.fd);
  c.fd = -1;
}

void AudioServer::check_local_streams() {
  std::lock_guard<std::mutex> lock(local_lock_);
  for (auto it = local_streams_.begin(); it != local_streams_.end();) {
    Mixer::Status s;
    if (!mixer_.status(it->id, s) || s.queued == 0) {
      mixer_.remove_stream(it->id);
      ps5_log_printf("%s", it->done_message.c_str());
      it = local_streams_.erase(it);
    } else {
      ++it;
    }
  }
}

}  // namespace prosperobrowser::audio
