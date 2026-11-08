// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/site_user_agents.h"

#include <algorithm>

#include "prosperoweb/core/url_normalizer.h"

namespace prosperoweb::core {

namespace {

struct PresetInfo {
  UaPreset preset;
  std::string_view id;
  std::string_view label;
  std::string_view user_agent;
};

// Update the version numbers here when they get old enough for sites to
// complain about an outdated browser.
constexpr PresetInfo kPresets[] = {
    {UaPreset::Default, "default", "Default", ""},
    {UaPreset::DesktopChrome, "desktop-chrome", "Desktop (Chrome)",
     "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
     "Chrome/140.0.0.0 Safari/537.36"},
    {UaPreset::Mobile, "mobile", "Mobile (iPhone)",
     "Mozilla/5.0 (iPhone; CPU iPhone OS 17_5 like Mac OS X) AppleWebKit/605.1.15 "
     "(KHTML, like Gecko) Version/17.5 Mobile/15E148 Safari/604.1"},
    {UaPreset::SmartTv, "tv", "Smart TV",
     "Mozilla/5.0 (SMART-TV; Linux; Tizen 6.0) AppleWebKit/537.36 (KHTML, like Gecko) "
     "76.0.3809.146/6.0 TV Safari/537.36"},
};

const PresetInfo& info(UaPreset preset) {
  for (const auto& p : kPresets) {
    if (p.preset == preset) {
      return p;
    }
  }
  return kPresets[0];
}

// The URL's host, or "" when it has none (about:, data:, file:///...).
std::string host_of(std::string_view url) {
  if (url.find("://") == std::string_view::npos) {
    return {};
  }
  std::string host = UrlNormalizer::display_host(url);
  // display_host() falls back to the whole URL when there is no authority.
  if (host.empty() || host.find_first_of("/:") != std::string::npos) {
    return {};
  }
  while (!host.empty() && host.back() == '.') {
    host.pop_back();
  }
  return host;
}

}  // namespace

std::string_view ua_preset_id(UaPreset preset) { return info(preset).id; }
std::string_view ua_preset_label(UaPreset preset) { return info(preset).label; }
std::string_view ua_preset_string(UaPreset preset) { return info(preset).user_agent; }

std::optional<UaPreset> ua_preset_from_id(std::string_view id) {
  for (const auto& p : kPresets) {
    if (p.id == id) {
      return p.preset;
    }
  }
  return std::nullopt;
}

bool SiteUserAgents::applies_to(std::string_view url) { return !host_of(url).empty(); }

UaPreset SiteUserAgents::preset_for(std::string_view url) const {
  const std::string host = host_of(url);
  if (host.empty()) {
    return UaPreset::Default;
  }
  const auto it = by_host_.find(host);
  return it == by_host_.end() ? UaPreset::Default : it->second;
}

std::string SiteUserAgents::user_agent_for(std::string_view url, std::string_view fallback) const {
  const UaPreset preset = preset_for(url);
  return std::string(preset == UaPreset::Default ? fallback : ua_preset_string(preset));
}

bool SiteUserAgents::set(std::string_view url, UaPreset preset) {
  const std::string host = host_of(url);
  if (host.empty()) {
    return false;
  }
  if (preset == UaPreset::Default) {
    by_host_.erase(host);
  } else {
    by_host_[host] = preset;
  }
  return true;
}

UaPreset SiteUserAgents::cycle(std::string_view url) {
  if (!applies_to(url)) {
    return UaPreset::Default;
  }
  const auto current = std::find(kUaPresets.begin(), kUaPresets.end(), preset_for(url));
  const auto next = (current == kUaPresets.end() || current + 1 == kUaPresets.end())
                        ? kUaPresets.front()
                        : *(current + 1);
  set(url, next);
  return next;
}

nlohmann::json SiteUserAgents::to_json() const {
  nlohmann::json out = nlohmann::json::object();
  for (const auto& [host, preset] : by_host_) {
    out[host] = std::string(ua_preset_id(preset));
  }
  return out;
}

bool SiteUserAgents::from_json(const nlohmann::json& j) {
  if (!j.is_object()) {
    return false;
  }
  by_host_.clear();
  for (const auto& [host, value] : j.items()) {
    if (host.empty() || !value.is_string()) {
      continue;
    }
    const auto preset = ua_preset_from_id(value.get<std::string>());
    // Normalise hand-edited keys ("WWW.Example.com." -> "www.example.com").
    const std::string h = host_of("https://" + host + "/");
    if (preset && *preset != UaPreset::Default && !h.empty()) {
      by_host_[h] = *preset;
    }
  }
  return true;
}

}  // namespace prosperoweb::core
