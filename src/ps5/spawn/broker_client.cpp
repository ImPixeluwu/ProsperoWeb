// SPDX-License-Identifier: GPL-3.0-or-later
#include "broker_client.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
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

std::string elfldr_payload_line(const std::string& elf, const std::string& args) {
  return "file:" + url_encode(elf) + "?args=" + url_encode(args) + "&pipe=0";
}

std::string elfldr_broker_args(const std::vector<std::string>& env) {
  std::string args = "--role=spawn-broker";
  for (const std::string& entry : env) {
    args += " " + websrv_escape_token("--env=" + entry);
  }
  return args;
}

namespace {

// A connected loopback TCP socket to host_port with send/receive timeouts,
// or -1 (*error says why).
int connect_ipv4(const std::string& host_port, int timeout_ms, std::string* error) {
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
    *error = "bad address '" + host_port + "' (want IPv4:port)";
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
  return fd;
}

bool send_all(int fd, const std::string& data, std::string* error) {
  size_t sent = 0;
  while (sent < data.size()) {
#ifdef MSG_NOSIGNAL
    const ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#else
    const ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
#endif
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      *error = std::string("send: ") + std::strerror(errno);
      return false;
    }
    sent += static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

int elfldr_send(const std::string& host_port, const std::string& line, int timeout_ms,
                std::string* reply, std::string* error) {
  reply->clear();
  const int fd = connect_ipv4(host_port, timeout_ms, error);
  if (fd < 0) {
    return -1;
  }
  if (!send_all(fd, line + "\n", error)) {
    close(fd);
    return -1;
  }
  shutdown(fd, SHUT_WR);
  char buf[512];
  while (reply->size() < 4096) {
    const ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0) {
      // A timeout after the payload started is not an error: elfldr keeps
      // the connection only while it loads.
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        *error = std::string("recv: ") + std::strerror(errno);
        close(fd);
        return -1;
      }
      break;
    }
    if (n == 0) {
      break;
    }
    reply->append(buf, static_cast<size_t>(n));
  }
  close(fd);
  // elfldr's messages end in "\n\r\0"; keep the text only.
  while (!reply->empty() && (reply->back() == '\0' || reply->back() == '\r' ||
                             reply->back() == '\n')) {
    reply->pop_back();
  }
  return reply->find("Error") != std::string::npos ? 1 : 0;
}

int http_get(const std::string& host_port, const std::string& target, int timeout_ms,
             std::string* error) {
  const int fd = connect_ipv4(host_port, timeout_ms, error);
  if (fd < 0) {
    return -1;
  }
  const std::string req = "GET " + target + " HTTP/1.0\r\nHost: " + host_port +
                          "\r\nConnection: close\r\n\r\n";
  if (!send_all(fd, req, error)) {
    close(fd);
    return -1;
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

bool tcp_port_open(const std::string& host_port, int timeout_ms, std::string* error) {
  std::string local_error;
  const int fd = connect_ipv4(host_port, timeout_ms, error ? error : &local_error);
  if (fd < 0) {
    return false;
  }
  close(fd);
  if (error) {
    error->clear();
  }
  return true;
}

std::vector<std::string> broker_loader_order(const std::string& loader, bool native) {
  if (loader == "elfldr" || loader == "websrv") {
    return {loader};
  }
  return native ? std::vector<std::string>{"elfldr", "websrv"}
                : std::vector<std::string>{"websrv", "elfldr"};
}

std::string etahen_launch_elfldr_request() {
  // struct IPCMessage { int32 magic; int32 cmd; int32 error; char msg[0x1000]; }
  constexpr size_t kMsgLen = 0x1000;
  std::string frame(3 * sizeof(int32_t) + kMsgLen, '\0');
  const int32_t header[3] = {static_cast<int32_t>(kEtahenIpcMagic),
                             static_cast<int32_t>(kEtahenCmdLaunchElfldr), 0};
  std::memcpy(&frame[0], header, sizeof(header));
  return frame;
}

int etahen_send(const std::string& socket_path, const std::string& request, int timeout_ms,
                std::string* reply_detail, std::string* error) {
  if (reply_detail) {
    reply_detail->clear();
  }
  if (socket_path.size() + 1 > sizeof(sockaddr_un::sun_path)) {
    *error = "socket path too long: " + socket_path;
    return -1;
  }
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    *error = std::string("socket: ") + std::strerror(errno);
    return -1;
  }
  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    *error = "connect " + socket_path + ": " + std::strerror(errno);
    close(fd);
    return -1;
  }
  if (!send_all(fd, request, error)) {
    close(fd);
    return -1;
  }
  // The daemon answers with one IPCMessage; the error field is at offset 8.
  std::string resp;
  char buf[512];
  while (resp.size() < request.size()) {
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
  if (resp.size() < 3 * sizeof(int32_t)) {
    *error = "short reply (" + std::to_string(resp.size()) + " bytes)";
    return -1;
  }
  int32_t reply_error = 0;
  std::memcpy(&reply_error, &resp[2 * sizeof(int32_t)], sizeof(reply_error));
  if (reply_detail && resp.size() > 3 * sizeof(int32_t)) {
    *reply_detail = resp.substr(3 * sizeof(int32_t), 64);
    const size_t nul = reply_detail->find('\0');
    if (nul != std::string::npos) {
      reply_detail->resize(nul);
    }
  }
  return reply_error == 0 ? 0 : 1;
}

long long poll_until_true(const std::function<bool()>& probe, long long total_ms,
                          long long interval_ms, const std::function<long long()>& clock_ms,
                          const std::function<void(long long)>& sleep_ms) {
  const auto now = clock_ms ? clock_ms : [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  const auto nap = sleep_ms ? sleep_ms : [](long long ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  };
  const long long start = now();
  for (;;) {
    if (probe()) {
      return now() - start;
    }
    const long long elapsed = now() - start;
    if (elapsed >= total_ms) {
      return -1;
    }
    const long long left = total_ms - elapsed;
    nap(interval_ms < left ? interval_ms : left);
  }
}

BrokerLaunchPolicy::Action BrokerLaunchPolicy::decide(long long now_ms) const {
  const bool in_flight =
      launches_done > 0 && last_launch_ms >= 0 && (now_ms - last_launch_ms) < launch_grace_ms;
  if (in_flight) {
    return kPoll;
  }
  if (launches_done >= max_launches) {
    // Burst spent: hold off until the cooldown passes, then allow one more so
    // a loader started later still gets a broker (slowly, never a rapid loop).
    if (last_launch_ms >= 0 && (now_ms - last_launch_ms) >= cooldown_ms) {
      return kLaunch;
    }
    return kGiveUp;
  }
  return kLaunch;
}

void BrokerLaunchPolicy::on_launched(long long now_ms) {
  // After the cooldown the burst restarts from one, so launches_done is a burst
  // counter, not an unbounded total.
  if (launches_done >= max_launches && last_launch_ms >= 0 &&
      (now_ms - last_launch_ms) >= cooldown_ms) {
    launches_done = 0;
  }
  launches_done++;
  last_launch_ms = now_ms;
}

void BrokerLaunchPolicy::on_connected() {
  launches_done = 0;
  last_launch_ms = -1;
}

}  // namespace prosperoweb::spawn
