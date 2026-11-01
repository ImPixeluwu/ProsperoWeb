// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PS5 add-ons backend. The ad blocker is the native WebKit content rule
// list (WKUserContentExtensionStore / WKContentRuleList, now that
// ENABLE(CONTENT_EXTENSIONS) is on): network `block` + `css-display-none`
// cosmetic rules compiled from EasyList/EasyPrivacy/uBlock/Peter-Lowe and
// attached to every content view - robust and never touching page JS. A small
// fail-open userscript handles only YouTube pre/mid-roll (json-prune), and
// SponsorBlock is a second userscript.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
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
  // addons_dir: bundled baseline (adblock-ads.json, adblock-cookie.json,
  //   adblock.js, sponsorblock.js, sources.json), read-only on the console.
  // cache_dir:  writable profile dir for the content-extension store and the
  //   freshly downloaded/recompiled rulesets.
  Ps5Addons(std::string addons_dir, std::string cache_dir, core::AddonsConfig initial);
  ~Ps5Addons() override;

  Ps5Addons(const Ps5Addons&) = delete;
  Ps5Addons& operator=(const Ps5Addons&) = delete;

  void start();
  void apply_to_view(WkWebView& view);
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
  struct CompileCtx;

  void ensure_filter(bool is_cookie);
  void compile_from_file(const std::string& json_path, bool is_cookie, bool from_update);
  void compile_json(const std::string& json, bool is_cookie, bool from_update);
  void adopt_filter(bool is_cookie, WKUserContentFilterRef filter);
  static void on_lookup(WKUserContentFilterRef filter, std::uint32_t result, void* ctx);
  static void on_compile(WKUserContentFilterRef filter, std::uint32_t result, void* ctx);
  void apply_filters();
  void apply_user_scripts();
  std::vector<std::string> build_user_scripts() const;
  std::string best_json_path(const char* cache_name, const char* bundled_name) const;
  void finish_update(bool ok);

  void start_update();
  void worker_run();

  std::string addons_dir_;
  std::string cache_dir_;
  core::AddonsConfig cfg_;
  std::string adblock_js_;
  std::string sponsorblock_js_;

  WKUserContentExtensionStoreRef store_ = nullptr;
  WKUserContentFilterRef ads_filter_ = nullptr;     // retained
  WKUserContentFilterRef cookie_filter_ = nullptr;  // retained

  std::vector<std::string> ads_sources_;
  std::vector<std::string> cookie_sources_;

  std::function<void(std::int64_t)> on_filters_updated_;

  std::thread worker_;
  std::mutex mutex_;
  std::atomic<bool> updating_{false};
  bool result_ready_ = false;        // guarded by mutex_
  bool result_ok_ = false;           // guarded by mutex_
  std::string fetched_ads_json_;     // guarded by mutex_ (WebKit rule-list JSON)
  std::string fetched_cookie_json_;  // guarded by mutex_
  std::int64_t last_updated_ = 0;
  std::int64_t last_check_ = 0;
  std::int64_t update_started_at_ = 0;
  int update_pending_ = 0;    // outstanding compiles for the current refresh
  bool update_ads_ok_ = false;
};

}  // namespace prosperoweb::frontend
