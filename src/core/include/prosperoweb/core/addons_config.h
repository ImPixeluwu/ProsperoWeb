// SPDX-License-Identifier: GPL-3.0-or-later
//
// The persisted state of the bundled add-ons (ad blocker, SponsorBlock,
// pop-up blocker), stored as the "addons" object of
// /data/prosperoweb/config.json. Parsing is
// tolerant: any missing key keeps the default below, so a config.json written
// before add-ons existed migrates to the documented defaults on load.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "prosperoweb/core/popup_policy.h"

namespace prosperoweb::core {

struct AdblockConfig {
  bool enabled = true;           // ad blocker is on by default
  bool cookie_banners = false;   // EasyList-Cookie sub-filter, off by default
  bool auto_update = true;       // refresh filter lists over the network
  std::int64_t last_updated = 0; // epoch seconds of the last successful update
};

struct SponsorBlockConfig {
  bool enabled = false;                           // off by default
  std::vector<std::string> categories = {"sponsor"};  // only "sponsor" on by default
};

struct AddonsConfig {
  AdblockConfig adblock;
  SponsorBlockConfig sponsorblock;
  PopupBlockerConfig popups;  // "popups": {"enabled", "allowed_sites"}

  // The SponsorBlock categories offered in the UI, in display order.
  static const std::vector<std::string>& all_sponsorblock_categories();
  static bool is_sponsorblock_category(const std::string& c);

  // Parse the "addons" object. Missing/invalid keys keep the defaults.
  static AddonsConfig from_json(const nlohmann::json& addons);
  // Read config.json's root object and pull out "addons" (defaults if absent).
  static AddonsConfig from_config_root(const nlohmann::json& root);
  nlohmann::json to_json() const;
};

}  // namespace prosperoweb::core
