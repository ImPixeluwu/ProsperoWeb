// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/remote/remote_service.h"

#include <random>

#include "prosperoweb/shell/shell_controller.h"

namespace prosperoweb::remote {

void system_random_bytes(unsigned char* out, std::size_t size) {
  static std::random_device device;
  for (std::size_t i = 0; i < size;) {
    auto v = device();
    for (std::size_t k = 0; k < sizeof v && i < size; ++k, ++i) {
      out[i] = static_cast<unsigned char>(v & 0xff);
      v >>= 8;
    }
  }
}

RemoteService::RemoteService() : RemoteService(Options{}) {}

RemoteService::RemoteService(Options options, Pairing::RandomBytes random)
    : options_(std::move(options)),
      api_(random ? std::move(random) : Pairing::RandomBytes(system_random_bytes)),
      server_([this](const HttpRequest& r) { return api_.handle(r); }, RemoteApi::kMaxBodyBytes) {}

RemoteService::~RemoteService() { server_.stop(); }

std::optional<shell::RemoteControl::Info> RemoteService::start(std::string& error) {
  if (!server_.running()) {
    api_.start_session();
    if (!server_.start(options_.bind_address, options_.port, error)) {
      return std::nullopt;
    }
    std::string host = options_.advertised_host;
    if (host.empty()) host = local_ipv4_address();
    if (host.empty()) host = "127.0.0.1";
    url_ = "http://" + host + ":" + std::to_string(server_.port());
  }
  return info();
}

void RemoteService::stop() {
  server_.stop();
  api_.start_session();  // forget the paired phones and queued commands
  url_.clear();
}

bool RemoteService::running() const { return server_.running(); }

std::optional<shell::RemoteControl::Info> RemoteService::info() const {
  if (!server_.running()) return std::nullopt;
  return Info{url_, api_.pin()};
}

std::size_t RemoteService::paired_devices() const {
  return server_.running() ? api_.devices() : 0;
}

void RemoteService::disconnect_all() { api_.revoke_all(); }

void RemoteService::pump(shell::ShellController& shell) {
  if (!server_.running()) return;
  for (const auto& command : api_.take_commands()) {
    shell.remote_command(command);
  }
  // The status is only polled by the phone every second or so.
  if (pumps_++ % 15 == 0) {
    api_.set_status(shell.remote_status());
  }
}

}  // namespace prosperoweb::remote
