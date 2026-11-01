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

#include "prosperoweb/core/adblock.h"
#include "prosperoweb/core/filter_update.h"
#include "ps5_log.h"
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

constexpr std::size_t kMaxDownloadBytes = 32u * 1024 * 1024;
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

// Build the ads / cookie filter-set JSON from downloaded list text, logging
// per-list counts. Runs on the worker thread (no WebKit calls).
std::string build_filter_json(const std::vector<std::pair<std::string, std::string>>& named_lists,
                              const char* label) {
  std::vector<std::string> texts;
  for (const auto& [name, text] : named_lists) {
    auto fs = core::adblock::build_filter_set({text});
    log_printf("addons: %s/%s: %zu lines -> %zu hosts, %zu generic, %zu domain rules, %zu skipped",
               label, name.c_str(), fs.stats.input_lines, fs.stats.hosts, fs.stats.selectors,
               fs.domain_rules.size(), fs.stats.skipped);
    texts.push_back(text);
  }
  auto merged = core::adblock::build_filter_set(texts);
  log_printf("addons: %s merged: %zu hosts, %zu generic selectors, %zu domain rules", label,
             merged.hosts.size(), merged.selectors.size(), merged.domain_rules.size());
  return merged.to_json();
}

// Merge a second filter-set JSON into the first (hosts/css/domainRules).
nlohmann::json merge_filter_json(const std::string& a, const std::string& b) {
  nlohmann::json ja = a.empty() ? nlohmann::json::object()
                                : nlohmann::json::parse(a, nullptr, false);
  if (!ja.is_object()) {
    ja = nlohmann::json::object();
  }
  if (b.empty()) {
    return ja;
  }
  nlohmann::json jb = nlohmann::json::parse(b, nullptr, false);
  if (!jb.is_object()) {
    return ja;
  }
  auto arr = [](nlohmann::json& o, const char* k) -> nlohmann::json& {
    if (!o.contains(k) || !o[k].is_array()) {
      o[k] = nlohmann::json::array();
    }
    return o[k];
  };
  for (auto& h : arr(jb, "hosts")) {
    arr(ja, "hosts").push_back(h);
  }
  for (auto& r : arr(jb, "domainRules")) {
    arr(ja, "domainRules").push_back(r);
  }
  std::string css = ja.value("css", std::string());
  std::string css_b = jb.value("css", std::string());
  if (!css_b.empty()) {
    css += (css.empty() ? "" : "\n") + css_b;
  }
  ja["css"] = css;
  return ja;
}

}  // namespace

Ps5Addons::Ps5Addons(std::string addons_dir, std::string cache_dir, core::AddonsConfig initial)
    : addons_dir_(std::move(addons_dir)), cache_dir_(std::move(cache_dir)), cfg_(std::move(initial)) {
  last_updated_ = cfg_.adblock.last_updated;
  make_dirs(cache_dir_);
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

Ps5Addons::~Ps5Addons() {
  if (worker_.joinable()) {
    worker_.join();
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
  if (adblock_js_.empty()) {
    log_line("addons: adblock.js missing; ad blocker disabled");
  }
  if (sponsorblock_js_.empty()) {
    log_line("addons: sponsorblock.js missing; SponsorBlock disabled");
  }

  ads_filter_json_ = read_file(best_json_path("adblock-ads.filters.json", "adblock-ads.filters.json"));
  cookie_filter_json_ =
      read_file(best_json_path("adblock-cookie.filters.json", "adblock-cookie.filters.json"));
  log_printf("addons: ads filter set %zu bytes, cookie filter set %zu bytes",
             ads_filter_json_.size(), cookie_filter_json_.size());

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

  apply_all();

  last_check_ = now_epoch();
  if (core::should_refresh_filters(cfg_.adblock.enabled, cfg_.adblock.auto_update, last_updated_,
                                   now_epoch())) {
    log_line("addons: filter lists stale, refreshing in the background");
    start_update();
  }
}

std::string Ps5Addons::adblock_config_js() const {
  nlohmann::json cfg = merge_filter_json(
      ads_filter_json_, cfg_.adblock.cookie_banners ? cookie_filter_json_ : std::string());
  cfg["enabled"] = cfg_.adblock.enabled;
  return "window.__PROSPEROWEB_AB = " + cfg.dump() + ";\n";
}

std::string Ps5Addons::sponsorblock_config_js() const {
  nlohmann::json cats = cfg_.sponsorblock.categories;
  nlohmann::json cfg = {{"enabled", cfg_.sponsorblock.enabled},
                        {"categories", cats},
                        {"api", "https://sponsor.ajay.app"}};
  return "window.__PROSPEROWEB_SB = " + cfg.dump() + ";\n";
}

std::vector<std::string> Ps5Addons::build_scripts() const {
  std::vector<std::string> scripts;
  if (!adblock_js_.empty()) {
    scripts.push_back(adblock_config_js() + adblock_js_);
  }
  if (cfg_.sponsorblock.enabled && !sponsorblock_js_.empty()) {
    scripts.push_back(sponsorblock_config_js() + sponsorblock_js_);
  }
  return scripts;
}

void Ps5Addons::apply_all() {
  auto scripts = build_scripts();
  // Live update for already-loaded pages: push the new config and re-apply.
  const std::string ab_live =
      "(function(c){if(window.__PROSPEROWEB_AB){Object.assign(window.__PROSPEROWEB_AB,c);"
      "if(window.__PROSPEROWEB_AB.apply)window.__PROSPEROWEB_AB.apply();}})(" +
      [&] {
        nlohmann::json cfg = merge_filter_json(
            ads_filter_json_, cfg_.adblock.cookie_banners ? cookie_filter_json_ : std::string());
        cfg["enabled"] = cfg_.adblock.enabled;
        return cfg.dump();
      }() +
      ");";
  nlohmann::json sb_cats = cfg_.sponsorblock.categories;
  const std::string sb_live =
      "if(window.__PROSPEROWEB_SB){window.__PROSPEROWEB_SB.enabled=" +
      std::string(cfg_.sponsorblock.enabled ? "true" : "false") +
      ";if(window.__PROSPEROWEB_SB.apply)window.__PROSPEROWEB_SB.apply(" + sb_cats.dump() + ");}";

  int views = 0;
  for (WkWebView* v : WkWebView::content_views()) {
    v->set_addon_user_scripts(scripts);
    v->run_javascript(ab_live);
    v->run_javascript(sb_live);
    ++views;
  }

  // Report the enforced counts (replaces the old content-filter log).
  nlohmann::json ab = merge_filter_json(
      ads_filter_json_, cfg_.adblock.cookie_banners ? cookie_filter_json_ : std::string());
  const std::size_t hosts = ab.contains("hosts") && ab["hosts"].is_array() ? ab["hosts"].size() : 0;
  const std::size_t rules =
      ab.contains("domainRules") && ab["domainRules"].is_array() ? ab["domainRules"].size() : 0;
  log_printf("addons: adblock %s -> injected into %d view(s): %zu blocked hosts, %zu domain rules",
             cfg_.adblock.enabled ? "ON" : "OFF", views, hosts, rules);
}

void Ps5Addons::apply_to_view(WkWebView& view) {
  view.set_addon_user_scripts(build_scripts());
}

void Ps5Addons::set_adblock(bool enabled, bool cookie_banners) {
  cfg_.adblock.enabled = enabled;
  cfg_.adblock.cookie_banners = cookie_banners;
  apply_all();
}

void Ps5Addons::set_sponsorblock(bool enabled, const std::vector<std::string>& categories) {
  cfg_.sponsorblock.enabled = enabled;
  cfg_.sponsorblock.categories = categories;
  apply_all();
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

  auto name_of = [](const std::string& url) {
    auto slash = url.find_last_of('/');
    return slash == std::string::npos ? url : url.substr(slash + 1);
  };

  bool ok = true;
  std::vector<std::pair<std::string, std::string>> ads_lists;
  for (const auto& u : ads_urls) {
    std::string body;
    if (!https_get(u, body)) {
      ok = false;
      break;
    }
    ads_lists.emplace_back(name_of(u), std::move(body));
  }

  std::string ads_json;
  std::string cookie_json;
  if (ok) {
    ads_json = build_filter_json(ads_lists, "ads");
    if (cfg_.adblock.cookie_banners && !cookie_urls.empty()) {
      std::vector<std::pair<std::string, std::string>> cookie_lists;
      for (const auto& u : cookie_urls) {
        std::string body;
        if (!https_get(u, body)) {
          ok = false;
          break;
        }
        cookie_lists.emplace_back(name_of(u), std::move(body));
      }
      if (ok) {
        cookie_json = build_filter_json(cookie_lists, "cookie");
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
      if (ok && !ads_json.empty()) {
        write_file(cache_dir_ + "/adblock-ads.filters.json", ads_json);
        ads_filter_json_ = std::move(ads_json);
        if (!cookie_json.empty()) {
          write_file(cache_dir_ + "/adblock-cookie.filters.json", cookie_json);
          cookie_filter_json_ = std::move(cookie_json);
        }
        apply_all();
        log_line("addons: filter lists refreshed and re-injected");
        finish_update(/*ok=*/true);
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
  }
  update_started_at_ = 0;
  updating_.store(false);
  if (on_filters_updated_) {
    on_filters_updated_(last_updated_);
  }
}

std::int64_t Ps5Addons::filters_last_updated() const { return last_updated_; }

bool Ps5Addons::filters_updating() const { return updating_.load(); }

}  // namespace prosperoweb::frontend
