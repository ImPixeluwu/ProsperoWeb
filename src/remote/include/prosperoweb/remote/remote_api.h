// SPDX-License-Identifier: GPL-3.0-or-later
//
// The remote control's HTTP API, without any sockets (RemoteServer feeds it
// requests from its thread; the shell drains the commands on its own):
//
//   GET  /                 the phone page (no token needed)
//   POST /api/pair         {"pin": "123456"} -> 200 {"token"} | 403 | 429
//   GET  /api/status       the shell's state (see ShellController::remote_status())
//   POST /api/command      a core::RemoteCommand as JSON, or an array of them
//
// /api/* except /api/pair need the token in the X-PW-Token header (401
// otherwise). Thread-safe.
#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "prosperoweb/core/remote_command.h"
#include "prosperoweb/remote/http.h"
#include "prosperoweb/remote/pairing.h"

namespace prosperoweb::remote {

class RemoteApi {
 public:
  static constexpr std::size_t kMaxBodyBytes = 64 * 1024;
  static constexpr std::size_t kMaxQueuedCommands = 256;

  explicit RemoteApi(Pairing::RandomBytes random);

  // A new pairing session: new PIN, no paired phones, empty queue.
  std::string start_session();
  std::string pin() const;
  std::size_t devices() const;
  void revoke_all();

  HttpResponse handle(const HttpRequest& request);

  // Commands received since the last call, oldest first (shell thread).
  std::vector<core::RemoteCommand> take_commands();
  // What GET /api/status returns (shell thread).
  void set_status(nlohmann::json status);

 private:
  bool authorized(const HttpRequest& request) const;  // with mutex_ held
  HttpResponse pair(const HttpRequest& request);
  HttpResponse command(const HttpRequest& request);

  mutable std::mutex mutex_;
  Pairing pairing_;
  std::deque<core::RemoteCommand> queue_;
  nlohmann::json status_ = nlohmann::json::object();
};

// The phone page served at "/".
const std::string& phone_page();

}  // namespace prosperoweb::remote
