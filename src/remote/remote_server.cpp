// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/remote/remote_server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <vector>

namespace prosperoweb::remote {

namespace {

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

bool set_nonblocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct Client {
  std::string in;
  std::string out;
  bool close_after_write = false;
  std::int64_t last_activity = 0;
};

}  // namespace

RemoteServer::RemoteServer(Handler handler, std::size_t max_body)
    : handler_(std::move(handler)), max_body_(max_body) {}

RemoteServer::~RemoteServer() { stop(); }

bool RemoteServer::start(const std::string& bind_address, int port, std::string& error) {
  if (running_) {
    return true;
  }
  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    error = std::string("socket: ") + std::strerror(errno);
    return false;
  }
  int one = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1) {
    error = "bad bind address " + bind_address;
    close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
      listen(listen_fd_, 16) != 0 || !set_nonblocking(listen_fd_) || pipe(wake_pipe_) != 0) {
    error = "cannot listen on " + bind_address + ":" + std::to_string(port) + ": " +
            std::strerror(errno);
    close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  socklen_t len = sizeof(addr);
  getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  port_ = ntohs(addr.sin_port);
  running_ = true;
  thread_ = std::thread([this] { run(); });
  return true;
}

void RemoteServer::stop() {
  if (!running_ && !thread_.joinable()) {
    return;
  }
  running_ = false;
  if (wake_pipe_[1] >= 0) {
    const char c = 1;
    (void)!write(wake_pipe_[1], &c, 1);
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  for (int& fd : wake_pipe_) {
    if (fd >= 0) close(fd);
    fd = -1;
  }
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    listen_fd_ = -1;
  }
}

void RemoteServer::run() {
  std::map<int, Client> clients;
  std::vector<pollfd> fds;
  char buf[16 * 1024];
  [[maybe_unused]] const int one = 1;
  while (running_) {
    fds.clear();
    fds.push_back({wake_pipe_[0], POLLIN, 0});
    fds.push_back({listen_fd_, POLLIN, 0});
    for (const auto& [fd, c] : clients) {
      fds.push_back({fd, static_cast<short>(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});
    }
    if (poll(fds.data(), static_cast<nfds_t>(fds.size()), 1000) < 0 && errno != EINTR) {
      break;
    }
    if (!running_ || (fds[0].revents & POLLIN)) {
      break;
    }
    const std::int64_t t = now_ms();
    if (fds[1].revents & POLLIN) {
      for (;;) {
        const int fd = accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) break;
        if (static_cast<int>(clients.size()) >= kMaxClients || !set_nonblocking(fd)) {
          close(fd);
          continue;
        }
#ifdef SO_NOSIGPIPE
        // FreeBSD/PS5 have no MSG_NOSIGNAL: a phone closing mid-response
        // must not kill the browser with SIGPIPE.
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        clients[fd].last_activity = t;
      }
    }
    std::vector<int> closed;
    for (std::size_t i = 2; i < fds.size(); ++i) {
      const int fd = fds[i].fd;
      Client& c = clients[fd];
      bool drop = (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
      if (!drop && (fds[i].revents & POLLIN)) {
        const ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n > 0) {
          c.in.append(buf, static_cast<std::size_t>(n));
          c.last_activity = t;
        } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
          drop = true;
        }
      }
      // Answer every complete request in the buffer, in order.
      while (!drop && !c.close_after_write && !c.in.empty()) {
        HttpRequest req;
        std::size_t used = 0;
        const ParseStatus st = parse_http_request(c.in, max_body_, req, used);
        if (st == ParseStatus::Incomplete) break;
        if (st != ParseStatus::Complete) {
          c.out += HttpResponse::json(st == ParseStatus::TooLarge ? 413 : 400,
                                      {{"error", "bad_request"}})
                       .serialize(false);
          c.close_after_write = true;
          c.in.clear();
          break;
        }
        c.in.erase(0, used);
        const bool keep = req.keep_alive();
        c.out += handler_(req).serialize(keep);
        if (!keep) c.close_after_write = true;
      }
      if (!drop && !c.out.empty()) {
        const ssize_t n = send(fd, c.out.data(), c.out.size(), kSendFlags);
        if (n > 0) {
          c.out.erase(0, static_cast<std::size_t>(n));
          c.last_activity = t;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
          drop = true;
        }
      }
      if (c.out.empty() && c.close_after_write) drop = true;
      if (t - c.last_activity > kIdleTimeoutMs) drop = true;
      if (drop) closed.push_back(fd);
    }
    for (const int fd : closed) {
      close(fd);
      clients.erase(fd);
    }
  }
  for (const auto& [fd, c] : clients) {
    close(fd);
  }
}

std::string local_ipv4_address() {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return {};
  }
  sockaddr_in remote{};
  remote.sin_family = AF_INET;
  remote.sin_port = htons(53);
  inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);  // only picks a route; nothing is sent
  std::string out;
  if (connect(fd, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) {
    sockaddr_in local{};
    socklen_t len = sizeof(local);
    char text[INET_ADDRSTRLEN] = {};
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len) == 0 &&
        local.sin_addr.s_addr != htonl(INADDR_ANY) &&
        inet_ntop(AF_INET, &local.sin_addr, text, sizeof text)) {
      out = text;
    }
  }
  close(fd);
  return out;
}

}  // namespace prosperoweb::remote
