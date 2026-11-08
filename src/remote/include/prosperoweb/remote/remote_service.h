// SPDX-License-Identifier: GPL-3.0-or-later
//
// The remote control as the frontends use it: RemoteServer + RemoteApi
// behind the shell::RemoteControl interface. The frontend owns one, passes
// it in ShellController::Config::remote_control and calls pump() every
// frame on the shell's thread.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "prosperoweb/remote/remote_api.h"
#include "prosperoweb/remote/remote_server.h"
#include "prosperoweb/shell/remote.h"

namespace prosperoweb::shell {
class ShellController;
}

namespace prosperoweb::remote {

class RemoteService final : public shell::RemoteControl {
 public:
  static constexpr int kDefaultPort = 17780;

  struct Options {
    std::string bind_address = "0.0.0.0";
    int port = kDefaultPort;  // 0: any free port (tests)
    // Host shown in the URL; empty: local_ipv4_address(), then "127.0.0.1".
    std::string advertised_host;
  };

  RemoteService();
  explicit RemoteService(Options options, Pairing::RandomBytes random = {});
  ~RemoteService() override;

  std::optional<Info> start(std::string& error) override;
  void stop() override;
  bool running() const override;
  std::optional<Info> info() const override;
  std::size_t paired_devices() const override;
  void disconnect_all() override;

  // Runs the phones' commands on the shell and refreshes /api/status.
  void pump(shell::ShellController& shell);

  int port() const { return server_.port(); }
  RemoteApi& api() { return api_; }

 private:
  Options options_;
  RemoteApi api_;
  RemoteServer server_;
  std::string url_;
  std::uint64_t pumps_ = 0;
};

// Cryptographically secure random bytes (std::random_device: getrandom on
// Linux, arc4random on FreeBSD/PS5 libc++).
void system_random_bytes(unsigned char* out, std::size_t size);

}  // namespace prosperoweb::remote
