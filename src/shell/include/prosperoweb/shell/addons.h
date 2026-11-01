// SPDX-License-Identifier: GPL-3.0-or-later
//
// Engine-side application of the bundled add-ons. ShellController owns the
// persisted state (core::AddonsConfig) and the Add-ons menu; the frontend
// implements this backend to actually apply the state to the web engine
// (WebKit content rule lists and user scripts on the PS5 port).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace prosperoweb::shell {

class AddonsBackend {
 public:
  virtual ~AddonsBackend() = default;

  // Apply the ad blocker: when enabled, the EasyList/EasyPrivacy ruleset is
  // on the content views; cookie_banners additionally applies the EasyList
  // Cookie List ruleset. Called at startup and on every toggle.
  virtual void set_adblock(bool enabled, bool cookie_banners) = 0;

  // Apply SponsorBlock: when enabled, the user script runs with the given
  // enabled categories (live-updated without reload). Empty categories while
  // enabled means nothing is skipped.
  virtual void set_sponsorblock(bool enabled, const std::vector<std::string>& categories) = 0;

  // Start a manual, background online refresh of the filter lists.
  virtual void update_filters_now() {}

  // Epoch seconds of the last successful filter-list update, 0 if never.
  virtual std::int64_t filters_last_updated() const { return 0; }

  // True while a background filter-list refresh is in progress.
  virtual bool filters_updating() const { return false; }
};

}  // namespace prosperoweb::shell
