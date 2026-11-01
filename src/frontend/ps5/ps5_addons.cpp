// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_addons.h"

#include <sys/stat.h>

#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <WebKit/WKString.h>
#include <WebKit/WKType.h>
#include <WebKit/WKUserContentExtensionStoreRef.h>

#include "prosperoweb/core/adblock.h"
#include "prosperoweb/core/filter_update.h"
#include "ps5_log.h"
#include "wk_helpers.h"
#include "wk_web_view.h"

namespace prosperoweb::frontend {

namespace {

std::int64_t now_epoch() { return static_cast<std::int64_t>(std::time(nullptr)); }

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::string();
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool write_file(const std::string& path, const std::string& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    return false;
  }
  out << data;
  return static_cast<bool>(out);
}

bool file_exists(const std::string& path) {
  struct stat st {};
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Append-with-cap write callback for libcurl.
constexpr std::size_t kMaxDownloadBytes = 32u * 1024 * 1024;
std::size_t curl_sink(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  const std::size_t n = size * nmemb;
  if (out->size() + n > kMaxDownloadBytes) {
    return 0;  // abort: too large
  }
  out->append(ptr, n);
  return n;
}

// HTTPS GET into `out`. Returns true on a 2xx with a body. Untrusted data:
// the caller only ever parses it.
bool https_get(const std::string& url, std::string& out) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    return false;
  }
  out.clear();
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_sink);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "ProsperoWeb-addons/1.0");
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");  // allow gzip
  if (const char* ca = std::getenv("PROSPEROWEB_CA_BUNDLE"); ca && ca[0]) {
    curl_easy_setopt(curl, CURLOPT_CAINFO, ca);
  }
  const CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  if (rc != CURLE_OK) {
    log_printf("addons: fetch %s failed: %s", url.c_str(), curl_easy_strerror(rc));
    return false;
  }
  if (status < 200 || status >= 300 || out.empty()) {
    log_printf("addons: fetch %s: http %ld, %zu bytes", url.c_str(), status, out.size());
    return false;
  }
  return true;
}

}  // namespace

struct Ps5Addons::CompileCtx {
  Ps5Addons* self;
  bool is_cookie;
  bool from_update;
};

Ps5Addons::Ps5Addons(std::string addons_dir, std::string cache_dir, core::AddonsConfig initial)
    : addons_dir_(std::move(addons_dir)),
      cache_dir_(std::move(cache_dir)),
      cfg_(std::move(initial)) {
  last_updated_ = cfg_.adblock.last_updated;
  ::mkdir(cache_dir_.c_str(), 0755);
  curl_global_init(CURL_GLOBAL_DEFAULT);
  auto path = make_wk_string(cache_dir_);
  store_ = WKUserContentExtensionStoreCreate(path.get());
}

Ps5Addons::~Ps5Addons() {
  if (worker_.joinable()) {
    worker_.join();
  }
  if (ads_filter_) {
    WKRelease(ads_filter_);
  }
  if (cookie_filter_) {
    WKRelease(cookie_filter_);
  }
  if (store_) {
    WKRelease(store_);
  }
  curl_global_cleanup();
}

std::string Ps5Addons::best_json_path(const char* cache_name, const char* bundled_name) const {
  const std::string cached = cache_dir_ + "/" + cache_name;
  if (file_exists(cached)) {
    return cached;
  }
  return addons_dir_ + "/" + bundled_name;
}

void Ps5Addons::start() {
  sponsorblock_js_ = read_file(addons_dir_ + "/sponsorblock.js");
  if (sponsorblock_js_.empty()) {
    log_line("addons: sponsorblock.js missing; SponsorBlock disabled");
  }

  // Read the refetch URLs from the bundled manifest (fall back to defaults).
  nlohmann::json manifest = nlohmann::json::parse(read_file(addons_dir_ + "/sources.json"),
                                                  nullptr, /*allow_exceptions=*/false);
  auto load_sources = [&](const char* key, std::vector<std::string>& out) {
    if (manifest.is_object()) {
      if (auto it = manifest.find(key); it != manifest.end() && it->is_object()) {
        if (auto s = it->find("sources"); s != it->end() && s->is_array()) {
          for (const auto& u : *s) {
            if (u.is_string()) {
              out.push_back(u.get<std::string>());
            }
          }
        }
      }
    }
  };
  load_sources("ads", ads_sources_);
  load_sources("cookie", cookie_sources_);

  log_printf("addons: adblock=%s cookie_banners=%s auto_update=%s sponsorblock=%s (%zu categories)",
             cfg_.adblock.enabled ? "on" : "off", cfg_.adblock.cookie_banners ? "on" : "off",
             cfg_.adblock.auto_update ? "on" : "off", cfg_.sponsorblock.enabled ? "on" : "off",
             cfg_.sponsorblock.categories.size());

  if (cfg_.adblock.enabled) {
    ensure_filter(/*is_cookie=*/false);
    if (cfg_.adblock.cookie_banners) {
      ensure_filter(/*is_cookie=*/true);
    }
  }
  apply_sponsorblock();

  last_check_ = now_epoch();
  if (core::should_refresh_filters(cfg_.adblock.enabled, cfg_.adblock.auto_update, last_updated_,
                                   now_epoch())) {
    log_line("addons: filter lists stale, refreshing in the background");
    start_update();
  }
}

void Ps5Addons::ensure_filter(bool is_cookie) {
  if (!store_) {
    return;
  }
  // Look the compiled filter up first (cached from a previous run / update);
  // compile the JSON only if the lookup misses.
  auto* ctx = new CompileCtx{this, is_cookie, /*from_update=*/false};
  auto id = make_wk_string(is_cookie ? "cookie" : "ads");
  WKUserContentExtensionStoreLookup(store_, id.get(), ctx, &Ps5Addons::on_lookup);
}

void Ps5Addons::on_lookup(WKUserContentFilterRef filter, std::uint32_t result, void* ctx_raw) {
  auto* ctx = static_cast<CompileCtx*>(ctx_raw);
  Ps5Addons* self = ctx->self;
  const bool is_cookie = ctx->is_cookie;
  delete ctx;
  if (result == kWKUserContentExtensionStoreSuccess && filter) {
    log_printf("addons: loaded cached %s ruleset", is_cookie ? "cookie" : "ads");
    self->adopt_filter(is_cookie, filter, /*from_update=*/false);
    return;
  }
  const std::string path = is_cookie
                               ? self->best_json_path("adblock-cookie.json", "adblock-cookie.json")
                               : self->best_json_path("adblock-ads.json", "adblock-ads.json");
  self->compile_from_file(path, is_cookie, /*from_update=*/false);
}

void Ps5Addons::compile_from_file(const std::string& json_path, bool is_cookie, bool from_update) {
  const std::string json = read_file(json_path);
  if (json.empty()) {
    log_printf("addons: %s ruleset missing at %s", is_cookie ? "cookie" : "ads", json_path.c_str());
    return;
  }
  compile_json(json, is_cookie, from_update);
}

void Ps5Addons::compile_json(const std::string& json, bool is_cookie, bool from_update) {
  if (!store_) {
    return;
  }
  log_printf("addons: compiling %s ruleset (%zu bytes)%s", is_cookie ? "cookie" : "ads",
             json.size(), from_update ? " from update" : "");
  auto* ctx = new CompileCtx{this, is_cookie, from_update};
  ++pending_compiles_;
  auto id = make_wk_string(is_cookie ? "cookie" : "ads");
  auto src = make_wk_string(json);
  WKUserContentExtensionStoreCompile(store_, id.get(), src.get(), ctx, &Ps5Addons::on_compile);
}

void Ps5Addons::on_compile(WKUserContentFilterRef filter, std::uint32_t result, void* ctx_raw) {
  auto* ctx = static_cast<CompileCtx*>(ctx_raw);
  Ps5Addons* self = ctx->self;
  const bool is_cookie = ctx->is_cookie;
  const bool from_update = ctx->from_update;
  delete ctx;
  if (self->pending_compiles_ > 0) {
    --self->pending_compiles_;
  }
  if (result != kWKUserContentExtensionStoreSuccess || !filter) {
    log_printf("addons: %s ruleset compile FAILED (result %u); keeping previous",
               is_cookie ? "cookie" : "ads", result);
    return;
  }
  log_printf("addons: %s ruleset compiled ok", is_cookie ? "cookie" : "ads");
  self->adopt_filter(is_cookie, filter, from_update);
}

void Ps5Addons::adopt_filter(bool is_cookie, WKUserContentFilterRef filter, bool from_update) {
  WKRetain(filter);
  WKUserContentFilterRef& slot = is_cookie ? cookie_filter_ : ads_filter_;
  if (slot) {
    WKRelease(slot);
  }
  slot = filter;
  apply_filters();
  if (from_update && !is_cookie) {
    // The ads ruleset is the one always present; treat its successful update
    // as the refresh completing.
    last_updated_ = now_epoch();
    if (on_filters_updated_) {
      on_filters_updated_(last_updated_);
    }
  }
}

void Ps5Addons::apply_filters() {
  std::vector<WKUserContentFilterRef> filters;
  if (cfg_.adblock.enabled && ads_filter_) {
    filters.push_back(ads_filter_);
  }
  if (cfg_.adblock.enabled && cfg_.adblock.cookie_banners && cookie_filter_) {
    filters.push_back(cookie_filter_);
  }
  for (WkWebView* v : WkWebView::content_views()) {
    v->set_content_filters(filters);
  }
}

std::string Ps5Addons::sponsorblock_config_js() const {
  nlohmann::json cats = cfg_.sponsorblock.categories;
  nlohmann::json cfg = {{"enabled", cfg_.sponsorblock.enabled},
                        {"categories", cats},
                        {"api", "https://sponsor.ajay.app"}};
  return "window.__PROSPEROWEB_SB = " + cfg.dump() + ";\n";
}

void Ps5Addons::apply_sponsorblock() {
  std::vector<std::string> scripts;
  if (cfg_.sponsorblock.enabled && !sponsorblock_js_.empty()) {
    scripts.push_back(sponsorblock_config_js() + sponsorblock_js_);
  }
  // Live-update the already-loaded pages: push the new enabled flag and
  // categories so a toggle takes effect without a reload.
  nlohmann::json cats = cfg_.sponsorblock.categories;
  const std::string live = "if(window.__PROSPEROWEB_SB){window.__PROSPEROWEB_SB.enabled=" +
                           std::string(cfg_.sponsorblock.enabled ? "true" : "false") +
                           ";window.__PROSPEROWEB_SB.apply(" + cats.dump() + ");}";
  for (WkWebView* v : WkWebView::content_views()) {
    v->set_addon_user_scripts(scripts);
    v->run_javascript(live);
  }
}

void Ps5Addons::apply_to_view(WkWebView& view) {
  std::vector<WKUserContentFilterRef> filters;
  if (cfg_.adblock.enabled && ads_filter_) {
    filters.push_back(ads_filter_);
  }
  if (cfg_.adblock.enabled && cfg_.adblock.cookie_banners && cookie_filter_) {
    filters.push_back(cookie_filter_);
  }
  view.set_content_filters(filters);
  if (cfg_.sponsorblock.enabled && !sponsorblock_js_.empty()) {
    view.set_addon_user_scripts({sponsorblock_config_js() + sponsorblock_js_});
  }
}

void Ps5Addons::set_adblock(bool enabled, bool cookie_banners) {
  cfg_.adblock.enabled = enabled;
  cfg_.adblock.cookie_banners = cookie_banners;
  if (enabled && !ads_filter_) {
    ensure_filter(/*is_cookie=*/false);
  }
  if (enabled && cookie_banners && !cookie_filter_) {
    ensure_filter(/*is_cookie=*/true);
  }
  apply_filters();
}

void Ps5Addons::set_sponsorblock(bool enabled, const std::vector<std::string>& categories) {
  cfg_.sponsorblock.enabled = enabled;
  cfg_.sponsorblock.categories = categories;
  apply_sponsorblock();
}

void Ps5Addons::update_filters_now() {
  if (!cfg_.adblock.enabled) {
    log_line("addons: update requested but the ad blocker is off");
    return;
  }
  start_update();
}

void Ps5Addons::start_update() {
  bool expected = false;
  if (!updating_.compare_exchange_strong(expected, true)) {
    return;  // already running
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    result_ready_ = false;
    result_ok_ = false;
    fetched_ads_json_.clear();
    fetched_cookie_json_.clear();
  }
  worker_ = std::thread([this] { worker_run(); });
}

void Ps5Addons::worker_run() {
  // Download and convert off the main thread. No WebKit calls here.
  std::vector<std::string> ads_urls = ads_sources_;
  if (ads_urls.empty()) {
    ads_urls = {"https://easylist.to/easylist/easylist.txt",
                "https://easylist.to/easylist/easyprivacy.txt"};
  }
  std::vector<std::string> cookie_urls = cookie_sources_;
  if (cookie_urls.empty()) {
    cookie_urls = {"https://secure.fanboy.co.nz/fanboy-cookiemonster.txt"};
  }

  bool ok = true;
  std::vector<std::string> ads_texts;
  for (const auto& u : ads_urls) {
    std::string body;
    if (!https_get(u, body)) {
      ok = false;
      break;
    }
    ads_texts.push_back(std::move(body));
  }

  std::string ads_json;
  std::string cookie_json;
  if (ok) {
    ads_json = core::adblock::convert(ads_texts).json;
    if (cfg_.adblock.cookie_banners) {
      std::vector<std::string> cookie_texts;
      for (const auto& u : cookie_urls) {
        std::string body;
        if (!https_get(u, body)) {
          ok = false;
          break;
        }
        cookie_texts.push_back(std::move(body));
      }
      if (ok) {
        cookie_json = core::adblock::convert(cookie_texts).json;
      }
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  result_ready_ = true;
  result_ok_ = ok;
  if (ok) {
    fetched_ads_json_ = std::move(ads_json);
    fetched_cookie_json_ = std::move(cookie_json);
  }
}

void Ps5Addons::pump() {
  // Finish a completed background download: compile + apply on the main thread.
  if (updating_.load()) {
    std::string ads_json;
    std::string cookie_json;
    bool ready = false;
    bool ok = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (result_ready_) {
        ready = true;
        ok = result_ok_;
        ads_json = std::move(fetched_ads_json_);
        cookie_json = std::move(fetched_cookie_json_);
        result_ready_ = false;
      }
    }
    if (ready) {
      if (worker_.joinable()) {
        worker_.join();
      }
      if (ok && !ads_json.empty()) {
        write_file(cache_dir_ + "/adblock-ads.json", ads_json);
        compile_json(ads_json, /*is_cookie=*/false, /*from_update=*/true);
        if (!cookie_json.empty()) {
          write_file(cache_dir_ + "/adblock-cookie.json", cookie_json);
          compile_json(cookie_json, /*is_cookie=*/true, /*from_update=*/true);
        }
        log_line("addons: background refresh fetched + recompiled filter lists");
      } else {
        log_line("addons: background refresh failed; keeping cached/bundled lists");
      }
      updating_.store(false);
    }
  }

  // Daily cadence while running.
  const std::int64_t now = now_epoch();
  if (now - last_check_ >= 3600) {  // check at most hourly
    last_check_ = now;
    if (!updating_.load() &&
        core::should_refresh_filters(cfg_.adblock.enabled, cfg_.adblock.auto_update, last_updated_,
                                     now)) {
      log_line("addons: daily cadence, refreshing filter lists");
      start_update();
    }
  }
}

std::int64_t Ps5Addons::filters_last_updated() const { return last_updated_; }

bool Ps5Addons::filters_updating() const { return updating_.load(); }

}  // namespace prosperoweb::frontend
