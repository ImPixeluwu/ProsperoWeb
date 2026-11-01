// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PS5 add-ons backend. Our WebKit build has ENABLE(CONTENT_EXTENSIONS)
// off (WKUserContentExtensionStoreCreate returns null), so the ad blocker is
// enforced by an injected userscript (cosmetic hiding + in-page network
// blocking + YouTube handling) built from EasyList/EasyPrivacy/uBlock filter
// lists, refreshed over the network on the console. SponsorBlock is a second
// bundled userscript.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "prosperoweb/core/addons_config.h"
#include "prosperoweb/shell/addons.h"

namespace prosperoweb::frontend {

class WkWebView;

class Ps5Addons final : public shell::AddonsBackend {
 public:
  // addons_dir: the bundled baseline (adblock.js, sponsorblock.js,
  //   adblock-ads.filters.json, adblock-cookie.filters.json, sources.json),
  //   read-only on the console.
  // cache_dir:  a writable profile directory for freshly downloaded/rebuilt
  //   filter sets.
  // initial:    the add-ons state from config.json.
  Ps5Addons(std::string addons_dir, std::string cache_dir, core::AddonsConfig initial);
  ~Ps5Addons() override;

  Ps5Addons(const Ps5Addons&) = delete;
  Ps5Addons& operator=(const Ps5Addons&) = delete;

  // Loads the scripts and filter sets, applies the initial state to existing
  // content views, and kicks a background refresh if the lists are stale.
  void start();

  // Called by the engine factory for every new content view.
  void apply_to_view(WkWebView& view);

  // Called every frame on the main thread: finishes a background refresh and
  // drives the daily refresh cadence.
  void pump();

  void set_on_filters_updated(std::function<void(std::int64_t)> cb) {
    on_filters_updated_ = std::move(cb);
  }

  // shell::AddonsBackend
  void set_adblock(bool enabled, bool cookie_banners) override;
  void set_sponsorblock(bool enabled, const std::vector<std::string>& categories) override;
  void update_filters_now() override;
  std::int64_t filters_last_updated() const override;
  bool filters_updating() const override;

 private:
  // Builds the extra user scripts (adblock + sponsorblock config + bodies) for
  // the current state.
  std::vector<std::string> build_scripts() const;
  // The window.__PROSPEROWEB_AB config (merged ads + cookie filter sets).
  std::string adblock_config_js() const;
  std::string sponsorblock_config_js() const;
  // Applies build_scripts() to every content view and live-updates the pages.
  void apply_all();
  std::string best_json_path(const char* cache_name, const char* bundled_name) const;

  void start_update();
  void worker_run();
  // Clears the updating state and refreshes the menu (every terminal outcome).
  void finish_update(bool ok);

  std::string addons_dir_;
  std::string cache_dir_;
  core::AddonsConfig cfg_;

  std::string adblock_js_;
  std::string sponsorblock_js_;
  std::string ads_filter_json_;     // {"hosts":..,"css":..,"domainRules":..}
  std::string cookie_filter_json_;  // same shape, for the cookie-banner lists

  std::vector<std::string> ads_sources_;
  std::vector<std::string> cookie_sources_;

  std::function<void(std::int64_t)> on_filters_updated_;

  // Background refresh.
  std::thread worker_;
  std::mutex mutex_;
  std::atomic<bool> updating_{false};
  bool result_ready_ = false;        // guarded by mutex_
  bool result_ok_ = false;           // guarded by mutex_
  std::string fetched_ads_json_;     // guarded by mutex_ (filter-set JSON)
  std::string fetched_cookie_json_;  // guarded by mutex_
  std::int64_t last_updated_ = 0;
  std::int64_t last_check_ = 0;
  std::int64_t update_started_at_ = 0;
};

}  // namespace prosperoweb::frontend
