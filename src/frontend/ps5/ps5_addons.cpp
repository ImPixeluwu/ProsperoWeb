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

Ps5Addons::Ps5Addons(std::string addons_dir, std::string cache_dir, core::AddonsConfig initial)
    : addons_dir_(std::move(addons_dir)), cache_dir_(std::move(cache_dir)), cfg_(std::move(initial)) {
  last_updated_ = cfg_.adblock.last_updated;
  ads_.is_cookie = false;
  ads_.prefix = "adblock-ads";
  cookie_.is_cookie = true;
  cookie_.prefix = "adblock-cookie";
  make_dirs(cache_dir_);
  curl_global_init(CURL_GLOBAL_DEFAULT);
  auto path = make_wk_string(cache_dir_);
  store_ = WKUserContentExtensionStoreCreate(path.get());
  log_printf("addons: content-extension store at %s: %s", cache_dir_.c_str(),
             store_ ? "ok" : "FAILED (null)");
}

Ps5Addons::~Ps5Addons() {
  if (worker_.joinable()) {
    worker_.join();
  }
  for (auto* f : ads_.filters) {
    if (f) {
      WKRelease(f);
    }
  }
  for (auto* f : cookie_.filters) {
    if (f) {
      WKRelease(f);
    }
  }
  if (store_) {
    WKRelease(store_);
  }
  curl_global_cleanup();
}

std::string Ps5Addons::identifier(bool is_cookie, int index) const {
  return std::string(is_cookie ? "cookie" : "ads") + std::to_string(index);
}

std::string Ps5Addons::chunk_path(const RuleSet& set, int index) const {
  const std::string name = set.prefix + "-" + std::to_string(index) + ".json";
  const std::string cached = cache_dir_ + "/" + name;
  if (file_exists(cached)) {
    return cached;
  }
  return addons_dir_ + "/" + name;
}

void Ps5Addons::start() {
  adblock_js_ = read_file(addons_dir_ + "/adblock.js");
  sponsorblock_js_ = read_file(addons_dir_ + "/sponsorblock.js");

  nlohmann::json manifest = nlohmann::json::parse(read_file(addons_dir_ + "/sources.json"), nullptr,
                                                  /*allow_exceptions=*/false);
  auto load_set = [&](const char* key, RuleSet& set) {
    if (manifest.is_object()) {
      if (auto it = manifest.find(key); it != manifest.end() && it->is_object()) {
        if (auto p = it->find("prefix"); p != it->end() && p->is_string()) {
          set.prefix = p->get<std::string>();
        }
        if (auto c = it->find("chunks"); c != it->end() && c->is_number_integer()) {
          set.chunks = c->get<int>();
        }
        if (auto s = it->find("sources"); s != it->end() && s->is_array()) {
          for (const auto& u : *s) {
            if (u.is_string()) {
              set.sources.push_back(u.get<std::string>());
            }
          }
        }
      }
    }
  };
  load_set("ads", ads_);
  load_set("cookie", cookie_);
  if (manifest.is_object()) {
    if (auto v = manifest.find("per_chunk"); v != manifest.end() && v->is_number_integer()) {
      per_chunk_ = v->get<std::size_t>();
    }
    if (auto v = manifest.find("max_total"); v != manifest.end() && v->is_number_integer()) {
      max_total_ = v->get<std::size_t>();
    }
  }
  ads_.filters.assign(static_cast<std::size_t>(ads_.chunks), nullptr);
  cookie_.filters.assign(static_cast<std::size_t>(cookie_.chunks), nullptr);

  log_printf("addons: adblock=%s cookie_banners=%s auto_update=%s sponsorblock=%s (%zu categories)",
             cfg_.adblock.enabled ? "on" : "off", cfg_.adblock.cookie_banners ? "on" : "off",
             cfg_.adblock.auto_update ? "on" : "off", cfg_.sponsorblock.enabled ? "on" : "off",
             cfg_.sponsorblock.categories.size());
  log_printf("addons: ads=%d chunk(s), cookie=%d chunk(s), per_chunk=%zu, max_total=%zu",
             ads_.chunks, cookie_.chunks, per_chunk_, max_total_);

  if (cfg_.adblock.enabled) {
    ensure_filters(/*is_cookie=*/false);
    if (cfg_.adblock.cookie_banners) {
      ensure_filters(/*is_cookie=*/true);
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

void Ps5Addons::ensure_filters(bool is_cookie) {
  if (!store_) {
    return;
  }
  RuleSet& set = set_for(is_cookie);
  for (int i = 0; i < set.chunks; ++i) {
    if (set.filters[static_cast<std::size_t>(i)]) {
      continue;  // already compiled/loaded
    }
    auto* ctx = new CompileCtx{this, is_cookie, i, /*from_update=*/false};
    auto id = make_wk_string(identifier(is_cookie, i));
    WKUserContentExtensionStoreLookup(store_, id.get(), ctx, &Ps5Addons::on_lookup);
  }
}

void Ps5Addons::on_lookup(WKUserContentFilterRef filter, std::uint32_t result, void* ctx_raw) {
  auto* ctx = static_cast<CompileCtx*>(ctx_raw);
  Ps5Addons* self = ctx->self;
  const bool is_cookie = ctx->is_cookie;
  const int index = ctx->index;
  delete ctx;
  if (result == kWKUserContentExtensionStoreSuccess && filter) {
    log_printf("addons: loaded cached %s chunk %d", is_cookie ? "cookie" : "ads", index);
    self->adopt_filter(is_cookie, index, filter);
    return;
  }
  const RuleSet& set = is_cookie ? self->cookie_ : self->ads_;
  self->compile_from_file(is_cookie, index, self->chunk_path(set, index), /*from_update=*/false);
}

void Ps5Addons::compile_from_file(bool is_cookie, int index, const std::string& json_path,
                                  bool from_update) {
  const std::string json = read_file(json_path);
  if (json.empty()) {
    log_printf("addons: %s chunk %d missing at %s", is_cookie ? "cookie" : "ads", index,
               json_path.c_str());
    if (from_update && update_pending_ > 0 && --update_pending_ == 0) {
      finish_update(update_ads_ok_);
    }
    return;
  }
  compile_json(is_cookie, index, json, from_update);
}

void Ps5Addons::compile_json(bool is_cookie, int index, const std::string& json, bool from_update) {
  if (!store_) {
    if (from_update && update_pending_ > 0 && --update_pending_ == 0) {
      finish_update(update_ads_ok_);
    }
    return;
  }
  log_printf("addons: compiling %s chunk %d (%zu rules, %zu bytes)%s", is_cookie ? "cookie" : "ads",
             index, count_rules(json), json.size(), from_update ? " from update" : "");
  auto* ctx = new CompileCtx{this, is_cookie, index, from_update};
  auto id = make_wk_string(identifier(is_cookie, index));
  auto src = make_wk_string(json);
  WKUserContentExtensionStoreCompile(store_, id.get(), src.get(), ctx, &Ps5Addons::on_compile);
}

void Ps5Addons::on_compile(WKUserContentFilterRef filter, std::uint32_t result, void* ctx_raw) {
  auto* ctx = static_cast<CompileCtx*>(ctx_raw);
  Ps5Addons* self = ctx->self;
  const bool is_cookie = ctx->is_cookie;
  const int index = ctx->index;
  const bool from_update = ctx->from_update;
  delete ctx;
  const bool ok = (result == kWKUserContentExtensionStoreSuccess && filter);
  if (ok) {
    log_printf("addons: %s chunk %d compiled ok", is_cookie ? "cookie" : "ads", index);
    self->adopt_filter(is_cookie, index, filter);
  } else {
    // A chunk that will not compile is skipped, never fatal.
    log_printf("addons: %s chunk %d compile FAILED (result %u); skipping",
               is_cookie ? "cookie" : "ads", index, result);
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

void Ps5Addons::adopt_filter(bool is_cookie, int index, WKUserContentFilterRef filter) {
  RuleSet& set = set_for(is_cookie);
  if (index < 0 || index >= static_cast<int>(set.filters.size())) {
    return;
  }
  WKRetain(filter);
  WKUserContentFilterRef& slot = set.filters[static_cast<std::size_t>(index)];
  if (slot) {
    WKRelease(slot);
  }
  slot = filter;
  apply_filters();
}

void Ps5Addons::collect_filters(std::vector<WKUserContentFilterRef>& out) const {
  if (cfg_.adblock.enabled) {
    for (auto* f : ads_.filters) {
      if (f) {
        out.push_back(f);
      }
    }
    if (cfg_.adblock.cookie_banners) {
      for (auto* f : cookie_.filters) {
        if (f) {
          out.push_back(f);
        }
      }
    }
  }
}

void Ps5Addons::apply_filters() {
  std::vector<WKUserContentFilterRef> filters;
  collect_filters(filters);
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
  collect_filters(filters);
  view.set_content_filters(filters);
  view.set_addon_user_scripts(build_user_scripts());
}

void Ps5Addons::set_adblock(bool enabled, bool cookie_banners) {
  cfg_.adblock.enabled = enabled;
  cfg_.adblock.cookie_banners = cookie_banners;
  if (enabled) {
    ensure_filters(/*is_cookie=*/false);
    if (cookie_banners) {
      ensure_filters(/*is_cookie=*/true);
    }
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
    fetched_ads_chunks_.clear();
    fetched_cookie_chunks_.clear();
  }
  update_started_at_ = now_epoch();
  log_line("addons: downloading filter lists in the background");
  worker_ = std::thread([this] { worker_run(); });
}

void Ps5Addons::worker_run() {
  // Download + chunk off the main thread (no WebKit calls here).
  auto fetch_all = [](const std::vector<std::string>& urls, bool& ok) {
    std::vector<std::string> texts;
    for (const auto& u : urls) {
      std::string body;
      if (!https_get(u, body)) {
        ok = false;
        return texts;
      }
      texts.push_back(std::move(body));
    }
    return texts;
  };

  bool ok = true;
  std::vector<std::string> ads_chunks;
  std::vector<std::string> cookie_chunks;
  std::vector<std::string> ads_texts = fetch_all(ads_.sources, ok);
  if (ok && !ads_texts.empty()) {
    ads_chunks = core::adblock::convert_chunked(ads_texts, per_chunk_, max_total_).chunks;
  }
  if (ok && cfg_.adblock.cookie_banners && !cookie_.sources.empty()) {
    std::vector<std::string> cookie_texts = fetch_all(cookie_.sources, ok);
    if (ok && !cookie_texts.empty()) {
      cookie_chunks = core::adblock::convert_chunked(cookie_texts, per_chunk_, max_total_).chunks;
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  result_ready_ = true;
  result_ok_ = ok;
  if (ok) {
    fetched_ads_chunks_ = std::move(ads_chunks);
    fetched_cookie_chunks_ = std::move(cookie_chunks);
  }
}

void Ps5Addons::pump() {
  if (updating_.load()) {
    std::vector<std::string> ads_chunks;
    std::vector<std::string> cookie_chunks;
    bool ready = false;
    bool ok = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (result_ready_) {
        ready = true;
        ok = result_ok_;
        ads_chunks = std::move(fetched_ads_chunks_);
        cookie_chunks = std::move(fetched_cookie_chunks_);
        result_ready_ = false;
      }
    }
    if (ready) {
      if (worker_.joinable()) {
        worker_.join();
      }
      if (ok && !ads_chunks.empty() && store_) {
        log_printf("addons: fetched filter lists, recompiling (%zu ads + %zu cookie chunks)",
                   ads_chunks.size(), cookie_chunks.size());
        update_ads_ok_ = false;
        update_pending_ = static_cast<int>(ads_chunks.size() + cookie_chunks.size());
        // New chunk counts: resize the filter vectors.
        ads_.chunks = static_cast<int>(ads_chunks.size());
        ads_.filters.resize(ads_chunks.size(), nullptr);
        cookie_.chunks = static_cast<int>(cookie_chunks.size());
        cookie_.filters.resize(cookie_chunks.size(), nullptr);
        for (std::size_t i = 0; i < ads_chunks.size(); ++i) {
          write_file(cache_dir_ + "/" + ads_.prefix + "-" + std::to_string(i) + ".json",
                     ads_chunks[i]);
          compile_json(/*is_cookie=*/false, static_cast<int>(i), ads_chunks[i],
                       /*from_update=*/true);
        }
        for (std::size_t i = 0; i < cookie_chunks.size(); ++i) {
          write_file(cache_dir_ + "/" + cookie_.prefix + "-" + std::to_string(i) + ".json",
                     cookie_chunks[i]);
          compile_json(/*is_cookie=*/true, static_cast<int>(i), cookie_chunks[i],
                       /*from_update=*/true);
        }
        if (update_pending_ == 0) {
          finish_update(false);  // nothing to compile
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
