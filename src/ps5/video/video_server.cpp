// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_server.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "FFmpegVideoOverlayWire.h"

extern "C" void ps5_log_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

namespace prosperoweb::video {

namespace wire = WebCore::FFmpegMedia::VideoWire;

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
      .count();
}

double ms_between(Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::milli>(b - a).count();
}

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

// Scaled frames larger than this are not drawn (a box far bigger than the
// screen; the page then shows a hole).
constexpr std::uint64_t kMaxScaledPixels = 3840ull * 2160ull;

// A File segment must be one of the clients' own files.
bool plausible_file(const std::string& path) {
  const std::string suffix = ".shm";
  return !path.empty() && path[0] == '/' && path.find("/../") == std::string::npos &&
         path.size() > suffix.size() &&
         path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

struct VideoServer::Connection {
  int fd = -1;
  std::uint32_t key = 0;  // 0 until the hello was accepted
  std::uint32_t pid = 0;
  std::uint32_t player = 0;
  wire::Parser parser{/*expectHello=*/true};
  std::vector<std::uint8_t> out;
  // Mapped segment (Shm/File transport).
  std::uint8_t* segment = nullptr;
  std::size_t segment_bytes = 0;
  wire::ShmLayout layout;
  bool tcp_logged = false;
  // The newest frame as received (kept to rescale on a resize).
  std::vector<std::uint32_t> source;
  int source_w = 0;
  int source_h = 0;
  // Scaling target, swapped with the player's front buffer.
  std::vector<std::uint32_t> back;
  std::vector<std::uint32_t> scratch;
  bool closed = false;
};

VideoServer::VideoServer(ServerOptions options) : options_(std::move(options)) {}

VideoServer::~VideoServer() { stop(); }

bool VideoServer::start() {
  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    ps5_log_printf("video overlay: socket: %s", std::strerror(errno));
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
    ps5_log_printf("video overlay: cannot listen on tcp 127.0.0.1:%d: %s", options_.port,
                   std::strerror(errno));
    close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  socklen_t len = sizeof(addr);
  getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  port_ = ntohs(addr.sin_port);
  set_nonblocking(listen_fd_);
  ps5_log_printf("video overlay: listening on tcp 127.0.0.1:%d (%s scaling)", port_,
                 options_.filter == display::ScaleFilter::Bilinear ? "bilinear" : "nearest");
  stop_ = false;
  stats_start_ms_ = now_ms();
  thread_ = std::thread([this] { run(); });
  return true;
}

void VideoServer::stop() {
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

VideoServer::Frames VideoServer::acquire(const PageKey& page) {
  Frames frames;
  frames.lock_ = std::unique_lock<std::mutex>(lock_);
  for (auto& [key, p] : players_) {
    if (p.pid != page.pid || p.page != page.page || !p.visible || !p.has_frame) {
      continue;
    }
    if (p.front_w != p.rect.w || p.front_h != p.rect.h) {
      continue;  // a resize is being rescaled: the page shows a hole meanwhile
    }
    frames.layers_.push_back(Layer{p.rect, p.front.data(), p.front_w, key});
    frames.new_frames_ += p.new_frames;
    p.new_frames = 0;
  }
  return frames;
}

std::vector<display::Rect> VideoServer::take_damage(const PageKey& page) {
  std::vector<display::Rect> out;
  std::lock_guard<std::mutex> lock(lock_);
  for (const Damage& d : damage_) {
    if (d.page == page) {
      out.push_back(d.rect);
    }
  }
  damage_.clear();
  return out;
}

std::size_t VideoServer::player_count() {
  std::lock_guard<std::mutex> lock(lock_);
  return players_.size();
}

void VideoServer::add_damage_locked(const Player& p) {
  if (p.visible && p.has_frame && !p.rect.empty()) {
    damage_.push_back({PageKey{p.pid, p.page}, p.rect});
  }
  // Bounded: a page nobody composes must not grow it forever.
  if (damage_.size() > 256) {
    damage_.erase(damage_.begin(), damage_.begin() + 128);
  }
}

void VideoServer::run() {
  std::vector<pollfd> fds;
  while (!stop_) {
    fds.clear();
    fds.push_back({listen_fd_, POLLIN, 0});
    for (auto& c : connections_) {
      fds.push_back({c->fd, POLLIN, 0});
    }
    const int ready = poll(fds.data(), fds.size(), 50);
    if (ready < 0 && errno != EINTR) {
      ps5_log_printf("video overlay: poll: %s", std::strerror(errno));
      return;
    }
    if (ready > 0) {
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
    }
    connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                                      [](const auto& c) { return c->closed; }),
                       connections_.end());
    if (options_.stats_interval_ms > 0 && now_ms() - stats_start_ms_ >= options_.stats_interval_ms) {
      log_stats();
    }
  }
}

void VideoServer::accept_clients() {
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
    int receive_buffer = 4 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
    auto c = std::make_unique<Connection>();
    c->fd = fd;
    connections_.push_back(std::move(c));
  }
}

bool VideoServer::service(Connection& c) {
  // Read everything available; a frame message is received straight into
  // the parser's buffer.
  while (true) {
    const std::size_t want = std::max<std::size_t>(c.parser.missingBytes(), 64 * 1024);
    std::uint8_t* buffer = c.parser.writable(want);
    const ssize_t got = recv(c.fd, buffer, want, 0);
    if (got > 0) {
      c.parser.commit(static_cast<std::size_t>(got));
      if (static_cast<std::size_t>(got) < want) {
        break;
      }
      continue;
    }
    if (got == 0) {
      // Process what arrived, then close.
      while (true) {
        const auto result = c.parser.next();
        if (result == wire::Parser::Result::NeedMore || result == wire::Parser::Result::Error) {
          break;
        }
        if (result == wire::Parser::Result::Message) {
          handle_message(c);
        }
      }
      return false;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      break;
    }
    return false;
  }
  while (true) {
    const auto result = c.parser.next();
    if (result == wire::Parser::Result::NeedMore) {
      return true;
    }
    if (result == wire::Parser::Result::Error) {
      ps5_log_printf("video overlay: player key %u: protocol error", c.key);
      return false;
    }
    if (result == wire::Parser::Result::Hello) {
      const wire::Hello& hello = c.parser.hello();
      if (hello.version != wire::version) {
        ps5_log_printf("video overlay: rejected a client with protocol version %u", hello.version);
        return false;
      }
      c.key = next_key_++;
      c.pid = hello.pid;
      c.player = hello.player;
      {
        std::lock_guard<std::mutex> lock(lock_);
        Player& p = players_[c.key];
        p.key = c.key;
        p.pid = c.pid;
        p.player = c.player;
      }
      c.out.clear();
      wire::appendAccepted(c.out, c.key);
      send(c.fd, c.out.data(), c.out.size(), send_flags());
      ps5_log_printf("video overlay: player key %u connected (pid %u player %u)", c.key, c.pid,
                     c.player);
      continue;
    }
    if (!handle_message(c)) {
      return false;
    }
  }
}

bool VideoServer::handle_message(Connection& c) {
  const auto& m = c.parser.message();
  switch (m.type) {
    case wire::Type::Shm: {
      const auto offer = wire::decodeShmOffer(m.payload, m.bytes);
      int error = EINVAL;
      bool ok = false;
      if (offer) {
        ok = map_segment(c, offer->name, offer->kind == wire::ShmKind::Named, offer->slots,
                         offer->slotBytes, error);
      }
      c.out.clear();
      wire::appendShmResult(c.out, wire::ShmResult{ok, ok ? 0u : static_cast<std::uint32_t>(error)});
      send(c.fd, c.out.data(), c.out.size(), send_flags());
      ps5_log_printf("video overlay: player key %u: %s %s %s%s%s", c.key,
                     offer && offer->kind == wire::ShmKind::File ? "file" : "shm",
                     offer ? offer->name.c_str() : "(malformed)", ok ? "mapped" : "refused: ",
                     ok ? "" : std::strerror(error),
                     ok ? "" : " (the client falls back)");
      return true;
    }
    case wire::Type::Geometry: {
      const auto g = wire::decodeGeometry(m.payload, m.bytes);
      if (!g) {
        return false;
      }
      bool rescale = false;
      {
        std::lock_guard<std::mutex> lock(lock_);
        Player& p = players_[c.key];
        add_damage_locked(p);  // where it was
        const display::Rect rect{g->x, g->y, g->width, g->height};
        rescale = rect.w != p.rect.w || rect.h != p.rect.h || (g->visible && !p.visible);
        p.rect = rect;
        p.page = g->pageId;
        p.visible = g->visible;
        p.fullscreen = g->fullscreen;
        if (!rescale) {
          add_damage_locked(p);  // where it is now
        }
      }
      if (rescale && !c.source.empty()) {
        render(c, false);
      }
      if (options_.on_update) {
        options_.on_update();
      }
      return true;
    }
    case wire::Type::Present: {
      const auto present = wire::decodePresent(m.payload, m.bytes);
      if (!present || !c.segment || !present->frame.valid(c.layout.slotBytes) ||
          present->frame.stride % 4) {
        return present.has_value();  // no segment yet: ignore; malformed: close
      }
      stats_.received++;
      const wire::FrameInfo& f = present->frame;
      c.source.resize(f.bytes() / 4);
      if (!wire::readSlot(c.segment, c.layout, present->slot, present->seq,
                          reinterpret_cast<std::uint8_t*>(c.source.data()), f.bytes())) {
        stats_.torn++;
        return true;
      }
      c.source_w = static_cast<int>(f.width);
      c.source_h = static_cast<int>(f.height);
      // Packed rows (the client always sends stride = width * 4).
      if (f.stride != f.width * 4) {
        for (std::uint32_t y = 1; y < f.height; y++) {
          std::memmove(c.source.data() + y * f.width, c.source.data() + y * (f.stride / 4),
                       f.width * 4);
        }
      }
      render(c, true);
      return true;
    }
    case wire::Type::Frame: {
      const std::uint8_t* pixels = nullptr;
      const auto f = wire::decodeFrame(m.payload, m.bytes, &pixels);
      if (!f || f->stride % 4) {
        return false;
      }
      if (!c.tcp_logged) {
        c.tcp_logged = true;
        ps5_log_printf("video overlay: player key %u: frames inline over tcp (%ux%u)", c.key,
                       f->width, f->height);
      }
      stats_.received++;
      stats_.tcp_bytes += m.bytes;
      c.source.resize(static_cast<std::size_t>(f->width) * f->height);
      for (std::uint32_t y = 0; y < f->height; y++) {
        std::memcpy(c.source.data() + static_cast<std::size_t>(y) * f->width,
                    pixels + static_cast<std::size_t>(y) * f->stride, f->width * 4);
      }
      c.source_w = static_cast<int>(f->width);
      c.source_h = static_cast<int>(f->height);
      render(c, true);
      return true;
    }
    case wire::Type::Clear: {
      std::lock_guard<std::mutex> lock(lock_);
      Player& p = players_[c.key];
      add_damage_locked(p);
      p.has_frame = false;
      c.source.clear();
      return true;
    }
    case wire::Type::Close:
      return false;
    default:
      return true;  // unknown: ignore (forward compatible)
  }
}

bool VideoServer::map_segment(Connection& c, const std::string& name, bool named,
                              std::uint32_t slots, std::uint32_t slot_bytes, int& error) {
  unmap_segment(c);
  if (!named && !plausible_file(name)) {
    error = EPERM;
    return false;
  }
  const int fd = named ? shm_open(name.c_str(), O_RDONLY, 0) : open(name.c_str(), O_RDONLY);
  if (fd < 0) {
    error = errno;
    return false;
  }
  const wire::ShmLayout layout{slots, slot_bytes};
  struct stat st{};
  if (fstat(fd, &st) != 0 || static_cast<std::uint64_t>(st.st_size) < layout.totalBytes()) {
    error = fstat(fd, &st) != 0 ? errno : EINVAL;
    close(fd);
    return false;
  }
  void* mapped = mmap(nullptr, layout.totalBytes(), PROT_READ, MAP_SHARED, fd, 0);
  error = errno;
  close(fd);
  if (mapped == MAP_FAILED) {
    return false;
  }
  auto* base = static_cast<std::uint8_t*>(mapped);
  if (!layout.matches(base, layout.totalBytes())) {
    munmap(mapped, layout.totalBytes());
    error = EINVAL;
    return false;
  }
  c.segment = base;
  c.segment_bytes = layout.totalBytes();
  c.layout = layout;
  return true;
}

void VideoServer::unmap_segment(Connection& c) {
  if (c.segment) {
    munmap(c.segment, c.segment_bytes);
  }
  c.segment = nullptr;
  c.segment_bytes = 0;
  c.layout = {};
}

void VideoServer::render(Connection& c, bool fresh) {
  display::Rect rect;
  bool visible;
  {
    std::lock_guard<std::mutex> lock(lock_);
    const Player& p = players_[c.key];
    rect = p.rect;
    visible = p.visible;
  }
  if (!visible || rect.empty() || rect.area() > kMaxScaledPixels || c.source.empty()) {
    return;  // shown again with the next frame or geometry
  }
  const auto t0 = Clock::now();
  c.back.resize(static_cast<std::size_t>(rect.area()));
  const display::ImageView src{c.source.data(), c.source_w, c.source_h, c.source_w};
  display::scale_image(src, c.back.data(), rect.w, rect.h, rect.w, options_.filter, c.scratch);
  const double ms = ms_between(t0, Clock::now());
  stats_.rendered++;
  stats_.scale_ms += ms;
  stats_.scale_max_ms = std::max(stats_.scale_max_ms, ms);
  stats_.src_w = c.source_w;
  stats_.src_h = c.source_h;
  stats_.dst_w = rect.w;
  stats_.dst_h = rect.h;
  {
    std::lock_guard<std::mutex> lock(lock_);
    Player& p = players_[c.key];
    if (p.rect.w != rect.w || p.rect.h != rect.h) {
      return;  // resized meanwhile; the geometry message rescales
    }
    p.front.swap(c.back);
    p.front_w = rect.w;
    p.front_h = rect.h;
    p.has_frame = true;
    if (fresh) {
      p.new_frames++;
    }
    add_damage_locked(p);
  }
  if (options_.on_update) {
    options_.on_update();
  }
}

void VideoServer::close_connection(Connection& c, const char* why) {
  if (c.closed) {
    return;
  }
  c.closed = true;
  unmap_segment(c);
  if (c.fd >= 0) {
    close(c.fd);
    c.fd = -1;
  }
  if (c.key) {
    {
      std::lock_guard<std::mutex> lock(lock_);
      auto it = players_.find(c.key);
      if (it != players_.end()) {
        add_damage_locked(it->second);
        players_.erase(it);
      }
    }
    ps5_log_printf("video overlay: player key %u closed%s%s", c.key, why ? ": " : "",
                   why ? why : "");
    if (options_.on_update) {
      options_.on_update();
    }
  }
}

void VideoServer::log_stats() {
  const std::int64_t now = now_ms();
  const double seconds = static_cast<double>(now - stats_start_ms_) / 1000.0;
  stats_start_ms_ = now;
  if (stats_.received || stats_.rendered) {
    ps5_log_printf(
        "video overlay %.1fs: frames in %.1f/s rendered %.1f/s torn %u | scale %dx%d -> %dx%d "
        "%.2f/%.2f ms | tcp %.1f MB/s | players %zu",
        seconds, stats_.received / seconds, stats_.rendered / seconds, stats_.torn, stats_.src_w,
        stats_.src_h, stats_.dst_w, stats_.dst_h,
        stats_.rendered ? stats_.scale_ms / stats_.rendered : 0.0, stats_.scale_max_ms,
        static_cast<double>(stats_.tcp_bytes) / seconds / 1e6, connections_.size());
  }
  stats_ = Stats{};
}

}  // namespace prosperoweb::video
