// SPDX-License-Identifier: GPL-3.0-or-later
#include "broker_client.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "broker_proto.h"

namespace prosperoweb::spawn {

using nlohmann::json;

namespace {

// Connects to one endpoint; returns the socket or -1 (errno set).
int connect_endpoint(const BrokerEndpoint& ep) {
  if (ep.tcp) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(ep.port));
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      return -1;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      const int err = errno;
      close(fd);
      errno = err;
      return -1;
    }
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (ep.path.empty() || ep.path.size() >= sizeof(addr.sun_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  std::memcpy(addr.sun_path, ep.path.c_str(), ep.path.size() + 1);
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    const int err = errno;
    close(fd);
    errno = err;
    return -1;
  }
  return fd;
}

std::vector<BrokerEndpoint> unix_only(std::string path) {
  BrokerEndpoint ep;
  ep.path = std::move(path);
  return {ep};
}

}  // namespace

BrokerClient::BrokerClient(std::vector<BrokerEndpoint> endpoints, ExitCallback on_exit)
    : endpoints_(std::move(endpoints)), on_exit_(std::move(on_exit)) {}

BrokerClient::BrokerClient(std::string socket_path, ExitCallback on_exit)
    : BrokerClient(unix_only(std::move(socket_path)), std::move(on_exit)) {}

BrokerClient::~BrokerClient() { disconnect(); }

bool BrokerClient::connected() {
  std::lock_guard<std::mutex> lock(mu_);
  return sock_ >= 0 && !dead_;
}

BrokerEndpoint BrokerClient::endpoint() {
  std::lock_guard<std::mutex> lock(mu_);
  return endpoint_;
}

bool BrokerClient::connect_once(std::string* failures) {
  if (connected()) {
    return true;
  }
  disconnect();  // a dead connection: join its reader first

  if (failures) {
    failures->clear();
  }
  int err = ENOENT;
  for (const auto& ep : endpoints_) {
    const int fd = connect_endpoint(ep);
    if (fd < 0) {
      err = errno;
      if (failures) {
        *failures += (failures->empty() ? "" : "; ") + ep.describe() + ": " +
                     std::strerror(err);
      }
      continue;
    }
    std::lock_guard<std::mutex> lock(mu_);
    sock_ = fd;
    endpoint_ = ep;
    dead_ = false;
    replies_.clear();
    reader_ = std::thread([this, fd] { reader_main(fd); });
    return true;
  }
  errno = err;
  return false;
}

bool BrokerClient::connect_wait(int timeout_ms, int interval_ms, std::string* failures) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  for (;;) {
    if (connect_once(failures)) {
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  }
}

void BrokerClient::disconnect() {
  int fd;
  std::thread reader;
  {
    std::lock_guard<std::mutex> lock(mu_);
    fd = sock_;
    reader.swap(reader_);
  }
  if (fd >= 0) {
    shutdown(fd, SHUT_RDWR);  // wakes the reader
  }
  if (reader.joinable()) {
    reader.join();
  }
  std::lock_guard<std::mutex> lock(mu_);
  if (fd >= 0) {
    close(fd);
  }
  sock_ = -1;
  dead_ = false;
  replies_.clear();
}

void BrokerClient::reader_main(int sock) {
  for (;;) {
    json msg;
    std::vector<int> fds;
    if (broker_recv_frame(sock, &msg, &fds) != BrokerRecv::kOk) {
      break;
    }
    for (int fd : fds) {
      close(fd);  // the broker never sends descriptors
    }
    if (broker_get_string(msg, "ev") == "exited") {
      if (on_exit_) {
        on_exit_(static_cast<int>(broker_get_int(msg, "pid", -1)),
                 static_cast<int>(broker_get_int(msg, "status", -1)));
      }
      continue;
    }
    std::lock_guard<std::mutex> lock(mu_);
    replies_.push_back(std::move(msg));
    cv_.notify_all();
  }
  std::lock_guard<std::mutex> lock(mu_);
  dead_ = true;
  cv_.notify_all();
}

bool BrokerClient::request(const json& msg, const int* fds, int nfds, json* reply,
                           int timeout_ms) {
  std::lock_guard<std::mutex> serial(request_mu_);
  int fd;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (sock_ < 0 || dead_) {
      errno = ENOTCONN;
      return false;
    }
    fd = sock_;
    replies_.clear();  // a late reply to a timed-out request
  }
  if (!broker_send_frame(fd, msg, fds, nfds)) {
    const int err = errno;
    disconnect();
    errno = err;
    return false;
  }
  std::unique_lock<std::mutex> lock(mu_);
  const bool got = cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [this] { return !replies_.empty() || dead_; });
  if (got && !replies_.empty()) {
    *reply = std::move(replies_.front());
    replies_.pop_front();
    return true;
  }
  lock.unlock();
  disconnect();
  errno = got ? ECONNRESET : ETIMEDOUT;
  return false;
}

std::string url_encode(const std::string& value) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : value) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 15];
    }
  }
  return out;
}

std::string websrv_escape_token(const std::string& token) {
  std::string out;
  for (char c : token) {
    if (c == ' ' || c == '\\') {
      out += '\\';
    }
    out += c;
  }
  return out;
}

std::string websrv_broker_target(const std::string& elf, const std::string& cwd,
                                 const std::string& args, const std::string& env) {
  return "/hbldr?path=" + url_encode(elf) + "&cwd=" + url_encode(cwd) +
         "&daemon=1&args=" + url_encode(args) + "&env=" + url_encode(env);
}

int http_get(const std::string& host_port, const std::string& target, int timeout_ms,
             std::string* error) {
  std::string host = host_port;
  int port = 80;
  const size_t colon = host_port.rfind(':');
  if (colon != std::string::npos) {
    host = host_port.substr(0, colon);
    port = std::atoi(host_port.c_str() + colon + 1);
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (port <= 0 || port > 65535 || inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    *error = "bad websrv address '" + host_port + "' (want IPv4:port)";
    return -1;
  }
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    *error = std::string("socket: ") + std::strerror(errno);
    return -1;
  }
  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    *error = std::string("connect ") + host_port + ": " + std::strerror(errno);
    close(fd);
    return -1;
  }
  const std::string req = "GET " + target + " HTTP/1.0\r\nHost: " + host_port +
                          "\r\nConnection: close\r\n\r\n";
  size_t sent = 0;
  while (sent < req.size()) {
#ifdef MSG_NOSIGNAL
    const ssize_t n = send(fd, req.data() + sent, req.size() - sent, MSG_NOSIGNAL);
#else
    const ssize_t n = send(fd, req.data() + sent, req.size() - sent, 0);
#endif
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      *error = std::string("send: ") + std::strerror(errno);
      close(fd);
      return -1;
    }
    sent += static_cast<size_t>(n);
  }
  // Only the status line matters; read until it is complete or EOF.
  std::string resp;
  char buf[512];
  while (resp.find("\r\n") == std::string::npos && resp.size() < 4096) {
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      *error = std::string("recv: ") + std::strerror(errno);
      close(fd);
      return -1;
    }
    if (n == 0) {
      break;
    }
    resp.append(buf, static_cast<size_t>(n));
  }
  close(fd);
  int code = 0;
  if (std::sscanf(resp.c_str(), "HTTP/%*d.%*d %d", &code) != 1 || code < 100) {
    *error = "no HTTP status line in the reply";
    return -1;
  }
  return code;
}

}  // namespace prosperoweb::spawn
