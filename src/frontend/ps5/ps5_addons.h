// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PS5 add-ons backend. The ad blocker is WebKit's native content rule
// list (ENABLE(CONTENT_EXTENSIONS) is on), but split into SEVERAL small
// compiled lists: the on-device content-extension compiler traps/OOMs on one
// huge list, so each chunk is kept small and compiled independently (like
// wBlock's several content blockers). A compile that fails is skipped, never
// fatal. A small fail-open userscript handles YouTube pre/mid-roll, and
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
  // One ruleset (ads or cookie), compiled as several small content rule lists.
  struct RuleSet {
    bool is_cookie = false;
    std::string prefix;                           // e.g. "adblock-ads"
    int chunks = 0;                               // number of chunk files
    std::vector<WKUserContentFilterRef> filters;  // compiled, by chunk index (may hold nullptr)
    std::vector<std::string> sources;             // refetch URLs
  };
  struct CompileCtx {
    Ps5Addons* self;
    bool is_cookie;
    int index;
    bool from_update;
  };

  RuleSet& set_for(bool is_cookie) { return is_cookie ? cookie_ : ads_; }
  void ensure_filters(bool is_cookie);
  void compile_from_file(bool is_cookie, int index, const std::string& json_path, bool from_update);
  void compile_json(bool is_cookie, int index, const std::string& json, bool from_update);
  void adopt_filter(bool is_cookie, int index, WKUserContentFilterRef filter);
  static void on_lookup(WKUserContentFilterRef filter, std::uint32_t result, void* ctx);
  static void on_compile(WKUserContentFilterRef filter, std::uint32_t result, void* ctx);
  std::string identifier(bool is_cookie, int index) const;
  std::string chunk_path(const RuleSet& set, int index) const;  // cache if present else bundled
  void collect_filters(std::vector<WKUserContentFilterRef>& out) const;
  void apply_filters();
  void apply_user_scripts();
  std::vector<std::string> build_user_scripts() const;
  void finish_update(bool ok);
  void start_update();
  void worker_run();

  std::string addons_dir_;
  std::string cache_dir_;
  core::AddonsConfig cfg_;
  std::string adblock_js_;
  std::string sponsorblock_js_;

  WKUserContentExtensionStoreRef store_ = nullptr;
  RuleSet ads_;
  RuleSet cookie_;
  std::size_t per_chunk_ = 20000;
  std::size_t max_total_ = 100000;

  std::function<void(std::int64_t)> on_filters_updated_;

  std::thread worker_;
  std::mutex mutex_;
  std::atomic<bool> updating_{false};
  bool result_ready_ = false;               // guarded by mutex_
  bool result_ok_ = false;                  // guarded by mutex_
  std::vector<std::string> fetched_ads_chunks_;     // guarded by mutex_
  std::vector<std::string> fetched_cookie_chunks_;  // guarded by mutex_
  std::int64_t last_updated_ = 0;
  std::int64_t last_check_ = 0;
  std::int64_t update_started_at_ = 0;
  int update_pending_ = 0;    // outstanding compiles for the current refresh
  bool update_ads_ok_ = false;
};

}  // namespace prosperoweb::frontend
