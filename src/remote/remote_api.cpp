// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/remote/remote_api.h"

namespace prosperoweb::remote {

namespace {

HttpResponse error(int status, const char* code) {
  return HttpResponse::json(status, {{"error", code}});
}

}  // namespace

RemoteApi::RemoteApi(Pairing::RandomBytes random) : pairing_(std::move(random)) {}

std::string RemoteApi::start_session() {
  std::lock_guard lock(mutex_);
  queue_.clear();
  return pairing_.reset();
}

std::string RemoteApi::pin() const {
  std::lock_guard lock(mutex_);
  return pairing_.pin();
}

std::size_t RemoteApi::devices() const {
  std::lock_guard lock(mutex_);
  return pairing_.devices();
}

void RemoteApi::revoke_all() {
  std::lock_guard lock(mutex_);
  pairing_.revoke_all();
}

bool RemoteApi::authorized(const HttpRequest& request) const {
  const std::string token = request.header("x-pw-token");
  return !token.empty() && pairing_.valid(token);
}

HttpResponse RemoteApi::handle(const HttpRequest& request) {
  if (request.path == "/" || request.path == "/index.html") {
    if (request.method != "GET") return error(405, "method");
    HttpResponse page;
    page.content_type = "text/html; charset=utf-8";
    page.body = phone_page();
    return page;
  }
  if (request.path == "/api/pair") {
    if (request.method != "POST") return error(405, "method");
    return pair(request);
  }
  if (request.path == "/api/status") {
    if (request.method != "GET") return error(405, "method");
    std::lock_guard lock(mutex_);
    if (!authorized(request)) return error(401, "unpaired");
    return HttpResponse::json(200, status_);
  }
  if (request.path == "/api/command") {
    if (request.method != "POST") return error(405, "method");
    return command(request);
  }
  return error(404, "not_found");
}

HttpResponse RemoteApi::pair(const HttpRequest& request) {
  const auto body = nlohmann::json::parse(request.body, nullptr, /*allow_exceptions=*/false);
  const std::string pin =
      body.is_object() && body.contains("pin") && body["pin"].is_string() ? body["pin"].get<std::string>()
                                                                           : std::string();
  std::lock_guard lock(mutex_);
  if (pairing_.locked()) return error(429, "locked");
  const auto token = pairing_.pair(pin);
  if (!token) return error(pairing_.locked() ? 429 : 403, pairing_.locked() ? "locked" : "wrong_pin");
  return HttpResponse::json(200, {{"token", *token}});
}

HttpResponse RemoteApi::command(const HttpRequest& request) {
  {
    std::lock_guard lock(mutex_);
    if (!authorized(request)) return error(401, "unpaired");
  }
  const auto body = nlohmann::json::parse(request.body, nullptr, /*allow_exceptions=*/false);
  std::vector<core::RemoteCommand> commands;
  const auto add = [&](const nlohmann::json& j) {
    const auto c = core::parse_remote_command(j);
    if (!c) return false;
    commands.push_back(*c);
    return true;
  };
  if (body.is_array()) {
    for (const auto& j : body) {
      if (!add(j)) return error(400, "bad_command");
    }
  } else if (!add(body)) {
    return error(400, "bad_command");
  }
  std::lock_guard lock(mutex_);
  if (queue_.size() + commands.size() > kMaxQueuedCommands) return error(503, "busy");
  queue_.insert(queue_.end(), commands.begin(), commands.end());
  return HttpResponse::json(200, {{"ok", true}});
}

std::vector<core::RemoteCommand> RemoteApi::take_commands() {
  std::lock_guard lock(mutex_);
  std::vector<core::RemoteCommand> out(queue_.begin(), queue_.end());
  queue_.clear();
  return out;
}

void RemoteApi::set_status(nlohmann::json status) {
  std::lock_guard lock(mutex_);
  status_ = std::move(status);
}

}  // namespace prosperoweb::remote
