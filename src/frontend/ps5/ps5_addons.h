// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PS5 add-ons backend: applies the ad blocker (WebKit content rule lists,
// compiled by WKUserContentExtensionStore) and the SponsorBlock user script to
// every content view, and refreshes the filter lists over the network on the
// console without a new browser release.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <WebKit/WKBase.h>

#include "prosperoweb/core/addons_config.h"
#include "prosperoweb/shell/addons.h"

namespace prosperoweb::frontend {

class WkWebView;

class Ps5Addons final : public shell::AddonsBackend {
 public:
  // addons_dir: the bundled baseline (adblock-ads.json, adblock-cookie.json,
  //   sponsorblock.js, sources.json), read-only on the console.
  // cache_dir:  a writable profile directory for the content-extension store
  //   and the freshly downloaded/converted rulesets.
  // initial:    the add-ons state from config.json.
  Ps5Addons(std::string addons_dir, std::string cache_dir, core::AddonsConfig initial);
  ~Ps5Addons() override;

  Ps5Addons(const Ps5Addons&) = delete;
  Ps5Addons& operator=(const Ps5Addons&) = delete;

  // Called once after the first content view exists: loads the SponsorBlock
  // script, compiles/loads the ad-block rulesets and applies the initial
  // state, then kicks a background refresh if the lists are stale.
  void start();

  // Called by the engine factory for every new content view.
  void apply_to_view(WkWebView& view);

  // Called every frame on the main thread: finishes a background refresh
  // (compile + apply) and drives the daily refresh cadence.
  void pump();

  // Set after construction: notified (main thread) when a refresh succeeds,
  // so the shell can record the timestamp and refresh the menu.
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
  // Compile-callback context (heap-allocated, freed in the callback).
  struct CompileCtx;

  void ensure_filter(bool is_cookie);
  void compile_from_file(const std::string& json_path, bool is_cookie, bool from_update);
  void compile_json(const std::string& json, bool is_cookie, bool from_update);
  void adopt_filter(bool is_cookie, WKUserContentFilterRef filter);
  // Called when all compiles of a background/manual refresh have finished
  // (success or failure): clears the updating state and refreshes the menu.
  void finish_update();
  // WKUserContentExtensionStore callbacks (ctx is a heap CompileCtx*).
  static void on_lookup(WKUserContentFilterRef filter, std::uint32_t result, void* ctx);
  static void on_compile(WKUserContentFilterRef filter, std::uint32_t result, void* ctx);
  void apply_filters();
  void apply_sponsorblock();
  std::string best_json_path(const char* cache_name, const char* bundled_name) const;
  std::string sponsorblock_config_js() const;

  // Background worker: download the source URLs and convert to rule-list JSON.
  void start_update();
  void worker_run();

  std::string addons_dir_;
  std::string cache_dir_;
  core::AddonsConfig cfg_;
  std::string sponsorblock_js_;

  WKUserContentExtensionStoreRef store_ = nullptr;
  WKUserContentFilterRef ads_filter_ = nullptr;     // retained
  WKUserContentFilterRef cookie_filter_ = nullptr;  // retained

  std::function<void(std::int64_t)> on_filters_updated_;

  // Source URLs read from sources.json (fallback to the known defaults).
  std::vector<std::string> ads_sources_;
  std::vector<std::string> cookie_sources_;

  // Background refresh.
  std::thread worker_;
  std::mutex mutex_;
  std::atomic<bool> updating_{false};
  bool result_ready_ = false;    // guarded by mutex_
  bool result_ok_ = false;       // guarded by mutex_
  std::string fetched_ads_json_;      // guarded by mutex_
  std::string fetched_cookie_json_;   // guarded by mutex_ (empty if not fetched)
  std::int64_t last_updated_ = 0;     // atomic-ish; written on main thread
  std::int64_t last_check_ = 0;       // last daily-cadence check (main thread)
  std::int64_t update_started_at_ = 0;  // when the current refresh began (0 = none)
  int pending_compiles_ = 0;          // outstanding async compiles (main thread)
  int update_pending_ = 0;            // outstanding compiles for the current refresh
  bool update_ads_ok_ = false;        // the refresh's ads compile succeeded
};

}  // namespace prosperoweb::frontend
