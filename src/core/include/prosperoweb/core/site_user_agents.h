// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace prosperoweb::core {

// The user-agent presets a site can be switched to from the menu.
enum class UaPreset : std::uint8_t {
  Default,        // the configured user agent (config.json, desktop Safari by default)
  DesktopChrome,  // Chrome on Windows, for sites that refuse Safari
  Mobile,         // Safari on iPhone: mobile layouts from the same engine family
  SmartTv,        // a Tizen smart TV, for TV interfaces such as youtube.com/tv
};

// Every preset in the order the menu cycles through them.
inline constexpr std::array<UaPreset, 4> kUaPresets = {UaPreset::Default, UaPreset::DesktopChrome,
                                                      UaPreset::Mobile, UaPreset::SmartTv};

// Stable id stored in user_agents.json: "default", "desktop-chrome",
// "mobile", "tv".
std::string_view ua_preset_id(UaPreset preset);
std::optional<UaPreset> ua_preset_from_id(std::string_view id);
// Menu label: "Default", "Desktop (Chrome)", "Mobile (iPhone)", "Smart TV".
std::string_view ua_preset_label(UaPreset preset);
// The preset's user-agent string; empty for Default (use the configured one).
std::string_view ua_preset_string(UaPreset preset);

// The user-agent preset chosen per host, saved by Session under
// kUserAgentsKey. Hosts on Default are not stored.
class SiteUserAgents {
 public:
  // The preset for url's host; Default when none is set or the URL has no
  // host (about:, data:, file:).
  UaPreset preset_for(std::string_view url) const;
  // The user agent to send for url: the preset's string, or fallback (the
  // configured user agent) for Default.
  std::string user_agent_for(std::string_view url, std::string_view fallback) const;

  // Sets url's host to preset (Default forgets it). Returns false when the
  // URL has no host.
  bool set(std::string_view url, UaPreset preset);
  // Moves url's host to the next preset (after the last comes Default) and
  // returns it. Default for URLs without a host.
  UaPreset cycle(std::string_view url);

  // Whether url has a host to keep a preset for.
  static bool applies_to(std::string_view url);

  std::size_t size() const { return by_host_.size(); }

  nlohmann::json to_json() const;
  // {"host": "mobile", ...}; unknown ids and non-strings are skipped. False
  // (and nothing changed) when j is not an object.
  bool from_json(const nlohmann::json& j);

 private:
  std::map<std::string, UaPreset, std::less<>> by_host_;
};

}  // namespace prosperoweb::core
