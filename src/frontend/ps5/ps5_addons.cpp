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

void make_dirs(const std::string& path) {
  std::string acc;
  for (char c : path) {
    acc.push_back(c);
    if (c == '/' && acc.size() > 1) {
      ::mkdir(acc.c_str(), 0755);
    }
  }
  if (!acc.empty() && acc.back() != '/') {
    ::mkdir(acc.c_str(), 0755);
  }
}

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

std::size_t count_rules(const std::string& json) {
  std::size_t n = 0;
  std::size_t pos = 0;
  const std::string key = "\"action\"";
  while ((pos = json.find(key, pos)) != std::string::npos) {
    ++n;
    pos += key.size();
  }
  return n;
}

constexpr std::size_t kMaxDownloadBytes = 64u * 1024 * 1024;
std::size_t curl_sink(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  const std::size_t n = size * nmemb;
  if (out->size() + n > kMaxDownloadBytes) {
    return 0;
  }
  out->append(ptr, n);
  return n;
}

bool https_get(const std::string& url, std::string& out) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    return false;
  }
  out.clear();
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 45L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curl_sink);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "ProsperoWeb-addons/1.0");
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
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
    : addons_dir_(std::move(addons_dir)), cache_dir_(std::move(cache_dir)), cfg_(std::move(initial)) {
  last_updated_ = cfg_.adblock.last_updated;
  make_dirs(cache_dir_);
  curl_global_init(CURL_GLOBAL_DEFAULT);
  auto path = make_wk_string(cache_dir_);
  store_ = WKUserContentExtensionStoreCreate(path.get());
  log_printf("addons: content-extension store at %s: %s", cache_dir_.c_str(),
             store_ ? "ok" : "FAILED (null; is ENABLE(CONTENT_EXTENSIONS) on?)");
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
  adblock_js_ = read_file(addons_dir_ + "/adblock.js");
  sponsorblock_js_ = read_file(addons_dir_ + "/sponsorblock.js");

  nlohmann::json manifest = nlohmann::json::parse(read_file(addons_dir_ + "/sources.json"), nullptr,
                                                  /*allow_exceptions=*/false);
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
  apply_user_scripts();

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
    self->adopt_filter(is_cookie, filter);
    return;
  }
  const std::string path =
      is_cookie ? self->best_json_path("adblock-cookie.json", "adblock-cookie.json")
                : self->best_json_path("adblock-ads.json", "adblock-ads.json");
  self->compile_from_file(path, is_cookie, /*from_update=*/false);
}

void Ps5Addons::compile_from_file(const std::string& json_path, bool is_cookie, bool from_update) {
  const std::string json = read_file(json_path);
  if (json.empty()) {
    log_printf("addons: %s ruleset missing at %s", is_cookie ? "cookie" : "ads", json_path.c_str());
    if (from_update && update_pending_ > 0 && --update_pending_ == 0) {
      finish_update(update_ads_ok_);
    }
    return;
  }
  compile_json(json, is_cookie, from_update);
}

void Ps5Addons::compile_json(const std::string& json, bool is_cookie, bool from_update) {
  if (!store_) {
    if (from_update && update_pending_ > 0 && --update_pending_ == 0) {
      finish_update(update_ads_ok_);
    }
    return;
  }
  log_printf("addons: compiling %s ruleset (%zu rules, %zu bytes)%s", is_cookie ? "cookie" : "ads",
             count_rules(json), json.size(), from_update ? " from update" : "");
  auto* ctx = new CompileCtx{this, is_cookie, from_update};
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
  const bool ok = (result == kWKUserContentExtensionStoreSuccess && filter);
  if (ok) {
    log_printf("addons: %s ruleset compiled ok", is_cookie ? "cookie" : "ads");
    self->adopt_filter(is_cookie, filter);
  } else {
    log_printf("addons: %s ruleset compile FAILED (result %u); keeping previous",
               is_cookie ? "cookie" : "ads", result);
  }
  if (from_update) {
    if (ok && !is_cookie) {
      self->update_ads_ok_ = true;
    }
    if (self->update_pending_ > 0 && --self->update_pending_ == 0) {
      self->finish_update(self->update_ads_ok_);
    }
  }
}

void Ps5Addons::adopt_filter(bool is_cookie, WKUserContentFilterRef filter) {
  WKRetain(filter);
  WKUserContentFilterRef& slot = is_cookie ? cookie_filter_ : ads_filter_;
  if (slot) {
    WKRelease(slot);
  }
  slot = filter;
  apply_filters();
}

void Ps5Addons::apply_filters() {
  std::vector<WKUserContentFilterRef> filters;
  if (cfg_.adblock.enabled && ads_filter_) {
    filters.push_back(ads_filter_);
  }
  if (cfg_.adblock.enabled && cfg_.adblock.cookie_banners && cookie_filter_) {
    filters.push_back(cookie_filter_);
  }
  int views = 0;
  for (WkWebView* v : WkWebView::content_views()) {
    v->set_content_filters(filters);
    ++views;
  }
  log_printf("addons: adblock %s -> %zu content filter(s) on %d view(s)",
             cfg_.adblock.enabled ? "ON" : "OFF", filters.size(), views);
}

std::vector<std::string> Ps5Addons::build_user_scripts() const {
  std::vector<std::string> scripts;
  // The YouTube json-prune script is injected whenever we have it, gated at
  // runtime by window.__PROSPEROWEB_AB.enabled, so a live toggle takes effect.
  if (!adblock_js_.empty()) {
    nlohmann::json ab = {{"enabled", cfg_.adblock.enabled}};
    scripts.push_back("window.__PROSPEROWEB_AB = " + ab.dump() + ";\n" + adblock_js_);
  }
  if (cfg_.sponsorblock.enabled && !sponsorblock_js_.empty()) {
    nlohmann::json cats = cfg_.sponsorblock.categories;
    nlohmann::json sb = {{"enabled", true}, {"categories", cats}, {"api", "https://sponsor.ajay.app"}};
    scripts.push_back("window.__PROSPEROWEB_SB = " + sb.dump() + ";\n" + sponsorblock_js_);
  }
  return scripts;
}

void Ps5Addons::apply_user_scripts() {
  auto scripts = build_user_scripts();
  nlohmann::json sb_cats = cfg_.sponsorblock.categories;
  const std::string ab_live =
      "if(window.__PROSPEROWEB_AB)window.__PROSPEROWEB_AB.enabled=" +
      std::string(cfg_.adblock.enabled ? "true" : "false") + ";";
  const std::string sb_live =
      "if(window.__PROSPEROWEB_SB){window.__PROSPEROWEB_SB.enabled=" +
      std::string(cfg_.sponsorblock.enabled ? "true" : "false") +
      ";if(window.__PROSPEROWEB_SB.apply)window.__PROSPEROWEB_SB.apply(" + sb_cats.dump() + ");}";
  for (WkWebView* v : WkWebView::content_views()) {
    v->set_addon_user_scripts(scripts);
    v->run_javascript(ab_live);
    v->run_javascript(sb_live);
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
  view.set_addon_user_scripts(build_user_scripts());
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
  apply_user_scripts();
}

void Ps5Addons::set_sponsorblock(bool enabled, const std::vector<std::string>& categories) {
  cfg_.sponsorblock.enabled = enabled;
  cfg_.sponsorblock.categories = categories;
  apply_user_scripts();
}

void Ps5Addons::update_filters_now() {
  if (!cfg_.adblock.enabled) {
    log_line("addons: manual update requested but the ad blocker is off");
    return;
  }
  log_line("addons: manual filter-list update requested");
  start_update();
}

void Ps5Addons::start_update() {
  bool expected = false;
  if (!updating_.compare_exchange_strong(expected, true)) {
    log_line("addons: a filter-list update is already in progress");
    return;
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
  update_started_at_ = now_epoch();
  log_line("addons: downloading filter lists in the background");
  worker_ = std::thread([this] { worker_run(); });
}

void Ps5Addons::worker_run() {
  std::vector<std::string> ads_urls = ads_sources_;
  if (ads_urls.empty()) {
    ads_urls = {"https://easylist.to/easylist/easylist.txt",
                "https://easylist.to/easylist/easyprivacy.txt"};
  }
  std::vector<std::string> cookie_urls = cookie_sources_;

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
    if (cfg_.adblock.cookie_banners && !cookie_urls.empty()) {
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
      if (ok && !ads_json.empty() && store_) {
        log_line("addons: fetched filter lists, recompiling");
        update_ads_ok_ = false;
        update_pending_ = 1;
        write_file(cache_dir_ + "/adblock-ads.json", ads_json);
        compile_json(ads_json, /*is_cookie=*/false, /*from_update=*/true);
        if (!cookie_json.empty()) {
          ++update_pending_;
          write_file(cache_dir_ + "/adblock-cookie.json", cookie_json);
          compile_json(cookie_json, /*is_cookie=*/true, /*from_update=*/true);
        }
      } else {
        log_line("addons: filter-list refresh failed; keeping cached/bundled lists");
        finish_update(/*ok=*/false);
      }
    } else if (update_started_at_ > 0 && now_epoch() - update_started_at_ > 240) {
      log_line("addons: filter-list update timed out; keeping cached/bundled lists");
      finish_update(/*ok=*/false);
    }
  }

  const std::int64_t now = now_epoch();
  if (now - last_check_ >= 3600) {
    last_check_ = now;
    if (!updating_.load() && core::should_refresh_filters(cfg_.adblock.enabled,
                                                          cfg_.adblock.auto_update, last_updated_,
                                                          now)) {
      log_line("addons: daily cadence, refreshing filter lists");
      start_update();
    }
  }
}

void Ps5Addons::finish_update(bool ok) {
  if (ok) {
    last_updated_ = now_epoch();
    log_line("addons: filter-list update complete");
  } else {
    log_line("addons: filter-list update finished with errors; kept previous rules");
  }
  update_started_at_ = 0;
  update_pending_ = 0;
  updating_.store(false);
  if (on_filters_updated_) {
    on_filters_updated_(last_updated_);
  }
}

std::int64_t Ps5Addons::filters_last_updated() const { return last_updated_; }

bool Ps5Addons::filters_updating() const { return updating_.load(); }

}  // namespace prosperoweb::frontend
