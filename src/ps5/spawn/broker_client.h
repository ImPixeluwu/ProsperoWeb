// SPDX-License-Identifier: GPL-3.0-or-later
//
// Client side of the spawn broker (the UI process), plus the minimal HTTP
// client that asks websrv to start the broker. Platform-independent.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "broker_proto.h"

namespace prosperoweb::spawn {

class BrokerClient {
 public:
  // Called on the reader thread for every {"ev":"exited"} notification.
  using ExitCallback = std::function<void(int pid, int status)>;

  // Tries the endpoints in order on every connection attempt.
  BrokerClient(std::vector<BrokerEndpoint> endpoints, ExitCallback on_exit);
  // A single AF_UNIX endpoint.
  BrokerClient(std::string socket_path, ExitCallback on_exit);
  ~BrokerClient();
  BrokerClient(const BrokerClient&) = delete;
  BrokerClient& operator=(const BrokerClient&) = delete;

  // One connection attempt per endpoint, in order. Returns false (errno of
  // the last endpoint) if nobody listens; *failures (may be null) then
  // lists every endpoint with its error.
  bool connect_once(std::string* failures = nullptr);
  // Retries connect_once() until it succeeds or timeout_ms passes.
  bool connect_wait(int timeout_ms, int interval_ms = 100,
                    std::string* failures = nullptr);
  bool connected();
  // The endpoint of the current connection (meaningful while connected):
  // over TCP descriptors cannot be attached, they are named (src_fds).
  BrokerEndpoint endpoint();
  void disconnect();

  // Sends msg (with fds attached) and waits for its reply. Requests are
  // serialised. Returns false if the connection failed or timed out; the
  // client is then disconnected.
  bool request(const nlohmann::json& msg, const int* fds, int nfds,
               nlohmann::json* reply, int timeout_ms);

 private:
  void reader_main(int sock);

  const std::vector<BrokerEndpoint> endpoints_;
  const ExitCallback on_exit_;

  std::mutex request_mu_;  // one request in flight
  std::mutex mu_;          // guards the fields below
  std::condition_variable cv_;
  int sock_ = -1;
  BrokerEndpoint endpoint_;
  bool dead_ = false;  // the reader saw EOF or an error
  std::deque<nlohmann::json> replies_;
  std::thread reader_;
};

// Percent-encodes a query-string value (everything but RFC 3986 unreserved).
std::string url_encode(const std::string& value);

// Escapes a token for websrv's argument splitter (sys.c args_split): a
// backslash before a space or another backslash.
std::string websrv_escape_token(const std::string& token);

// Builds the websrv request that starts the broker as a daemon payload:
//   /hbldr?path=<elf>&cwd=<cwd>&daemon=1&args=<args>&env=<env>
// websrv splits args and env on spaces (backslash escapes a space); args'
// first token becomes the broker's argv[0] and process name.
std::string websrv_broker_target(const std::string& elf, const std::string& cwd,
                                 const std::string& args, const std::string& env);

// Plain HTTP/1.0 GET over TCP. host_port is "a.b.c.d:port" (an IPv4
// literal; the port defaults to 80). Returns the HTTP status code, or -1
// on a connection or protocol error (*error describes it).
int http_get(const std::string& host_port, const std::string& target, int timeout_ms,
             std::string* error);

}  // namespace prosperoweb::spawn
