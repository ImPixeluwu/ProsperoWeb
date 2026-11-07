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

class Ps5Addons final : public shell::AddonsBackend, public shell::UserScriptsBackend {
 public:
  // Result of a detached background fetch. Held by shared_ptr so the worker
  // can outlive this object (abandoned on the watchdog) without a use-after-
  // free: the worker touches ONLY this state plus values captured by copy,
  // never `this`. Public so the file-local worker function can use it.
  struct WorkerState {
    std::mutex mutex;
    bool done = false;
    bool ok = false;
  };

  Ps5Addons(std::string addons_dir, std::string cache_dir, core::AddonsConfig initial);
  ~Ps5Addons() override;

  Ps5Addons(const Ps5Addons&) = delete;
  Ps5Addons& operator=(const Ps5Addons&) = delete;

  void start();
  // The shared user content controller every content view uses; the add-ons
  // attach to it once. base_scripts are re-added before the add-on scripts
  // when rebuilding the controller's scripts. Call before start().
  void attach_controller(WKUserContentControllerRef controller,
                         std::vector<std::string> base_scripts) {
    controller_ = controller;
    base_scripts_ = std::move(base_scripts);
  }
  void pump();

  void set_on_filters_updated(std::function<void(std::int64_t)> cb) {
    on_filters_updated_ = std::move(cb);
  }

  // shell::AddonsBackend
  void set_adblock(bool enabled, bool cookie_banners) override;
  void set_sponsorblock(bool enabled, const std::vector<std::string>& categories) override;
  // shell::UserScriptsBackend: the user's own scripts, injected after the
  // bundled add-ons on the shared controller.
  void set_user_scripts(std::vector<std::string> sources) override;
  void update_filters_now() override;
  std::int64_t filters_last_updated() const override;
  bool filters_updating() const override;

 private:
  // One ruleset (ads or cookie), compiled as several small content rule lists
  // ONE AT A TIME (serial) to bound peak memory.
  struct RuleSet {
    bool is_cookie = false;
    std::string prefix;                           // e.g. "adblock-ads"
    int chunks = 0;                               // number of chunk files
    std::vector<WKUserContentFilterRef> filters;  // compiled, by chunk index (may hold nullptr)
    std::vector<std::string> sources;             // refetch URLs
    int cursor = 0;                               // next chunk to process
    bool processing = false;                      // a serial walk is in flight
  };
  struct CompileCtx {
    Ps5Addons* self;
    bool is_cookie;
    int index;
  };

  RuleSet& set_for(bool is_cookie) { return is_cookie ? cookie_ : ads_; }
  // Begin a serial walk of a set's chunks (startup compile + the single attach
  // per launch). One compile in flight at a time to bound memory.
  void start_set(bool is_cookie);
  void process_next(bool is_cookie);
  void advance(bool is_cookie);
  void on_set_done(bool is_cookie);
  void compile_json(bool is_cookie, int index, const std::string& json);
  void adopt_filter(bool is_cookie, int index, WKUserContentFilterRef filter);
  static void on_compile(WKUserContentFilterRef filter, std::uint32_t result, void* ctx);
  std::string identifier(bool is_cookie, int index) const;
  std::string chunk_path(const RuleSet& set, int index) const;  // cache if present else bundled
  void collect_filters(std::vector<WKUserContentFilterRef>& out) const;
  std::size_t attached_total() const;  // compiled+retained lists across both sets
  void apply_filters();
  // Process RSS in MB (-1 if unavailable); logged per step. True if above the
  // backstop limit (stop attaching more).
  long rss_mb() const;
  void log_mem(const char* step) const;
  void apply_user_scripts();
  std::vector<std::string> build_user_scripts() const;
  void finish_update(bool ok);
  void start_update();

  std::string addons_dir_;
  std::string cache_dir_;
  core::AddonsConfig cfg_;
  std::string adblock_js_;
  std::string sponsorblock_js_;
  std::vector<std::string> user_scripts_;  // core::userscript_wrapper() output

  WKUserContentExtensionStoreRef store_ = nullptr;
  WKUserContentControllerRef controller_ = nullptr;  // shared, owned by the factory
  std::vector<std::string> base_scripts_;            // re-added before add-on scripts
  RuleSet ads_;
  RuleSet cookie_;
  std::size_t per_chunk_ = 20000;
  std::size_t max_total_ = 20000;
  // Hard cap on attached content rule lists. Each one hands a SharedMemory
  // descriptor to the WebProcess and the sandbox's handle budget is ~ONE for
  // these ~20000-rule lists (2nd handover logs "no descriptor left" at
  // startup). Default 1 so the single attached list always has a working
  // handle and really filters. Tunable via sources.json.
  std::size_t max_lists_ = 1;
  // Backstop: stop attaching more chunks if process RSS exceeds this (MB).
  long rss_limit_mb_ = 1400;

  std::function<void(std::int64_t)> on_filters_updated_;

  std::atomic<bool> updating_{false};
  std::shared_ptr<WorkerState> worker_state_;  // the current detached fetch
  std::int64_t last_updated_ = 0;
  std::int64_t last_check_ = 0;
  std::int64_t update_started_at_ = 0;
};

}  // namespace prosperoweb::frontend
