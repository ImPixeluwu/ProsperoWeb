// SPDX-License-Identifier: GPL-3.0-or-later
//
// A small HTTP/1.1 server on POSIX sockets for the remote control: one
// background thread, poll(), non-blocking sockets, keep-alive connections
// (the phone's trackpad sends many small requests). Every complete request
// goes to the handler on that thread. Works on Linux and on the PS5's
// FreeBSD-based libc.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "prosperoweb/remote/http.h"

namespace prosperoweb::remote {

class RemoteServer {
 public:
  using Handler = std::function<HttpResponse(const HttpRequest&)>;

  static constexpr int kMaxClients = 32;
  static constexpr int kIdleTimeoutMs = 15000;

  RemoteServer(Handler handler, std::size_t max_body);
  ~RemoteServer();

  RemoteServer(const RemoteServer&) = delete;
  RemoteServer& operator=(const RemoteServer&) = delete;

  // Listens on bind_address:port ("0.0.0.0" for the local network, port 0
  // for any free port) and starts the thread. False, with error set, when
  // the socket cannot be bound.
  bool start(const std::string& bind_address, int port, std::string& error);
  void stop();
  bool running() const { return running_; }
  int port() const { return port_; }

 private:
  void run();

  Handler handler_;
  std::size_t max_body_;
  int listen_fd_ = -1;
  int wake_pipe_[2] = {-1, -1};
  int port_ = 0;
  std::atomic<bool> running_{false};
  std::thread thread_;
};

// The console's IPv4 address on the local network ("" when unknown). Uses a
// UDP socket's route choice; no packet is sent.
std::string local_ipv4_address();

}  // namespace prosperoweb::remote
