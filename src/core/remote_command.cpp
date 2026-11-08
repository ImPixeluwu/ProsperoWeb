// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/remote_command.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace prosperoweb::core {

namespace {

constexpr std::array<std::pair<std::string_view, RemoteAction>, 12> kActions = {{
    {"back", RemoteAction::Back},
    {"forward", RemoteAction::Forward},
    {"reload", RemoteAction::Reload},
    {"prev_tab", RemoteAction::PrevTab},
    {"next_tab", RemoteAction::NextTab},
    {"new_tab", RemoteAction::NewTab},
    {"close_tab", RemoteAction::CloseTab},
    {"page_up", RemoteAction::PageUp},
    {"page_down", RemoteAction::PageDown},
    {"fullscreen", RemoteAction::Fullscreen},
    {"menu", RemoteAction::Menu},
    {"escape", RemoteAction::Escape},
}};

std::optional<float> delta(const nlohmann::json& j, const char* key) {
  const auto it = j.find(key);
  if (it == j.end()) {
    return 0.0f;
  }
  if (!it->is_number()) {
    return std::nullopt;
  }
  const double v = it->get<double>();
  if (!std::isfinite(v)) {
    return std::nullopt;
  }
  return std::clamp(static_cast<float>(v), -kMaxRemoteDelta, kMaxRemoteDelta);
}

std::optional<std::string> text_of(const nlohmann::json& j, bool allow_empty) {
  const auto it = j.find("text");
  if (it == j.end() || !it->is_string()) {
    return std::nullopt;
  }
  std::string text = it->get<std::string>();
  if ((text.empty() && !allow_empty) || text.size() > kMaxRemoteTextBytes) {
    return std::nullopt;
  }
  return text;
}

bool flag(const nlohmann::json& j, const char* key) {
  const auto it = j.find(key);
  return it != j.end() && it->is_boolean() && it->get<bool>();
}

}  // namespace

std::optional<RemoteAction> remote_action_from_name(std::string_view name) {
  for (const auto& [n, a] : kActions) {
    if (n == name) return a;
  }
  return std::nullopt;
}

std::optional<RemoteCommand> parse_remote_command(const nlohmann::json& j) {
  if (!j.is_object()) {
    return std::nullopt;
  }
  const auto type_it = j.find("type");
  if (type_it == j.end() || !type_it->is_string()) {
    return std::nullopt;
  }
  const std::string type = type_it->get<std::string>();
  RemoteCommand c;
  if (type == "open" || type == "text") {
    c.type = type == "open" ? RemoteCommand::Type::OpenUrl : RemoteCommand::Type::Text;
    c.new_tab = flag(j, "new_tab");
    c.submit = flag(j, "submit");
    // An empty Text is only a bare Enter press.
    auto text = text_of(j, c.type == RemoteCommand::Type::Text && c.submit);
    if (!text) return std::nullopt;
    c.text = std::move(*text);
    return c;
  }
  if (type == "pointer" || type == "scroll") {
    const auto dx = delta(j, "dx");
    const auto dy = delta(j, "dy");
    if (!dx || !dy) return std::nullopt;
    c.type = type == "pointer" ? RemoteCommand::Type::Pointer : RemoteCommand::Type::Scroll;
    c.dx = *dx;
    c.dy = *dy;
    return c;
  }
  if (type == "click") {
    c.type = RemoteCommand::Type::Click;
    return c;
  }
  if (type == "action") {
    const auto it = j.find("name");
    if (it == j.end() || !it->is_string()) return std::nullopt;
    const auto action = remote_action_from_name(it->get<std::string>());
    if (!action) return std::nullopt;
    c.type = RemoteCommand::Type::Action;
    c.action = *action;
    return c;
  }
  return std::nullopt;
}

}  // namespace prosperoweb::core
