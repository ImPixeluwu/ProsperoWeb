// SPDX-License-Identifier: GPL-3.0-or-later
//
// A command sent by a paired phone through the remote control (src/remote).
// Parsed from the JSON body of POST /api/command; the shell runs it on its
// own thread (ShellController::remote_command()).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace prosperoweb::core {

enum class RemoteAction : std::uint8_t {
  Back,
  Forward,
  Reload,
  PrevTab,
  NextTab,
  NewTab,
  CloseTab,
  PageUp,
  PageDown,
  Fullscreen,
  Menu,
  Escape,  // Circle: leave a menu, notice or fullscreen
};

struct RemoteCommand {
  enum class Type : std::uint8_t {
    OpenUrl,  // {type:'open', text, new_tab}      address-bar input
    Text,     // {type:'text', text, submit}       into the focused page field
    Pointer,  // {type:'pointer', dx, dy}          move the pointer (window px)
    Click,    // {type:'click'}                    Cross at the pointer
    Scroll,   // {type:'scroll', dx, dy}           scroll the page (px)
    Action,   // {type:'action', name}             see RemoteAction
  };
  Type type = Type::Click;
  std::string text;
  bool new_tab = false;
  bool submit = false;  // Text: press Enter after inserting it
  float dx = 0.0f;
  float dy = 0.0f;
  RemoteAction action = RemoteAction::Back;
};

// Limits applied by parse_remote_command().
inline constexpr std::size_t kMaxRemoteTextBytes = 8192;
inline constexpr float kMaxRemoteDelta = 4000.0f;

// nullopt for anything malformed: unknown type or action, missing or empty
// text (a Text with submit may be empty: a bare Enter), text longer than
// kMaxRemoteTextBytes, non-numeric deltas. Deltas are clamped to
// +-kMaxRemoteDelta.
std::optional<RemoteCommand> parse_remote_command(const nlohmann::json& j);

// "back", "forward", "reload", "prev_tab", "next_tab", "new_tab",
// "close_tab", "page_up", "page_down", "fullscreen", "menu", "escape".
std::optional<RemoteAction> remote_action_from_name(std::string_view name);

}  // namespace prosperoweb::core
