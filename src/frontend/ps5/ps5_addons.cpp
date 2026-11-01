// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5_addons.h"

#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>

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
    // Serial walk: ads chunks one at a time, then cookie (chained in
    // on_set_done) if that sub-toggle is on. Bounds peak memory.
    start_set(/*is_cookie=*/false, /*from_update=*/false);
  }
  apply_user_scripts();

  last_check_ = now_epoch();
  if (core::should_refresh_filters(cfg_.adblock.enabled, cfg_.adblock.auto_update, last_updated_,
                                   now_epoch())) {
    log_line("addons: filter lists stale, refreshing in the background");
    start_update();
  }
}

long Ps5Addons::rss_mb() const {
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
  struct kinfo_proc kp {};
  size_t len = sizeof(kp);
  if (sysctl(mib, 4, &kp, &len, nullptr, 0) != 0 || len == 0) {
    return -1;
  }
  const long page = sysconf(_SC_PAGESIZE);
  if (page <= 0) {
    return -1;
  }
  return static_cast<long>(kp.ki_rssize) * (page / 1024) / 1024;
}

void Ps5Addons::log_mem(const char* step) const {
  const long rss = rss_mb();
  if (rss >= 0) {
    log_printf("addons: mem after %s: RSS %ld MB", step, rss);
  }
}

void Ps5Addons::start_set(bool is_cookie, bool from_update) {
  RuleSet& set = set_for(is_cookie);
  if (set.processing) {
    return;  // a serial walk is already in flight for this set
  }
  set.cursor = 0;
  set.from_update = from_update;
  set.processing = true;
  process_next(is_cookie);
}

void Ps5Addons::process_next(bool is_cookie) {
  RuleSet& set = set_for(is_cookie);
  if (set.cursor >= set.chunks) {
    set.processing = false;
    on_set_done(is_cookie);
    return;
  }
  // Memory backstop: never OOM the UIProcess - stop attaching more chunks.
  const long rss = rss_mb();
  if (rss > 0 && rss >= rss_limit_mb_) {
    log_printf("addons: RSS %ld MB >= limit %ld MB; stopping %s at %d chunk(s)", rss, rss_limit_mb_,
               is_cookie ? "cookie" : "ads", set.cursor);
    set.processing = false;
    on_set_done(is_cookie);
    return;
  }
  const int i = set.cursor;
  if (set.from_update) {
    std::string json =
        (i < static_cast<int>(set.pending.size())) ? std::move(set.pending[i]) : std::string();
    if (i < static_cast<int>(set.pending.size())) {
      set.pending[i].clear();
      set.pending[i].shrink_to_fit();
    }
    write_file(cache_dir_ + "/" + set.prefix + "-" + std::to_string(i) + ".json", json);
    compile_json(is_cookie, i, json, /*from_update=*/true);  // -> on_compile -> advance
    return;
  }
  if (!store_) {
    set.processing = false;
    return;
  }
  auto* ctx = new CompileCtx{this, is_cookie, i, /*from_update=*/false};
  auto id = make_wk_string(identifier(is_cookie, i));
  WKUserContentExtensionStoreLookup(store_, id.get(), ctx, &Ps5Addons::on_lookup);
}

void Ps5Addons::advance(bool is_cookie) {
  RuleSet& set = set_for(is_cookie);
  ++set.cursor;
  process_next(is_cookie);
}

void Ps5Addons::on_set_done(bool is_cookie) {
  RuleSet& set = set_for(is_cookie);
  log_printf("addons: %s set done (%d chunk(s) processed, %zu attached)",
             is_cookie ? "cookie" : "ads", set.cursor,
             [&] {
               std::size_t n = 0;
               for (auto* f : set.filters) {
                 if (f) {
                   ++n;
                 }
               }
               return n;
             }());
  const bool was_update = set.from_update;
  set.from_update = false;
  if (!was_update) {
    // Startup: after ads, process the cookie set if that sub-toggle is on.
    if (!is_cookie && cfg_.adblock.enabled && cfg_.adblock.cookie_banners && !cookie_.processing) {
      start_set(/*is_cookie=*/true, /*from_update=*/false);
    }
    return;
  }
  // Refresh: ads first, then cookie, then report completion.
  if (!is_cookie && cfg_.adblock.cookie_banners && !cookie_.pending.empty()) {
    start_set(/*is_cookie=*/true, /*from_update=*/true);
    return;
  }
  finish_update(update_ads_ok_);
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
    self->log_mem("attach");
    self->advance(is_cookie);
    return;
  }
  // Lookup miss: compile the chunk from the cache/bundled file (async).
  const RuleSet& set = is_cookie ? self->cookie_ : self->ads_;
  const std::string json = read_file(self->chunk_path(set, index));
  if (json.empty()) {
    log_printf("addons: %s chunk %d missing; skipping", is_cookie ? "cookie" : "ads", index);
    self->advance(is_cookie);
    return;
  }
  self->compile_json(is_cookie, index, json, /*from_update=*/false);  // -> on_compile -> advance
}

void Ps5Addons::compile_json(bool is_cookie, int index, const std::string& json, bool from_update) {
  if (!store_ || json.empty()) {
    advance(is_cookie);
    return;
  }
  log_printf("addons: compiling %s chunk %d (%zu rules, %zu bytes)%s", is_cookie ? "cookie" : "ads",
             index, count_rules(json), json.size(), from_update ? " from update" : "");
  auto* ctx = new CompileCtx{this, is_cookie, index, from_update};
  auto id = make_wk_string(identifier(is_cookie, index));
  auto src = make_wk_string(json);  // WebKit copies the source; our std::string frees on return
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
    self->adopt_filter(is_cookie, index, filter);  // attaches incrementally
    self->log_mem("attach");
    if (from_update && !is_cookie) {
      self->update_ads_ok_ = true;
    }
  } else {
    // A chunk that will not compile is skipped, never fatal.
    log_printf("addons: %s chunk %d compile FAILED (result %u); skipping",
               is_cookie ? "cookie" : "ads", index, result);
  }
  self->advance(is_cookie);
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
  auto needs_compile = [](const RuleSet& s) {
    for (auto* f : s.filters) {
      if (!f) {
        return s.chunks > 0;
      }
    }
    return false;
  };
  if (enabled && !ads_.processing && needs_compile(ads_)) {
    start_set(/*is_cookie=*/false, /*from_update=*/false);  // cookie chains after ads
  } else if (enabled && cookie_banners && !cookie_.processing && needs_compile(cookie_)) {
    start_set(/*is_cookie=*/true, /*from_update=*/false);
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
        // Hand the new chunks to the serial walker (ads first, cookie chained);
        // it recompiles one at a time and finish_update() fires when done.
        ads_.chunks = static_cast<int>(ads_chunks.size());
        ads_.filters.resize(ads_chunks.size(), nullptr);
        ads_.pending = std::move(ads_chunks);
        cookie_.chunks = static_cast<int>(cookie_chunks.size());
        cookie_.filters.resize(cookie_chunks.size(), nullptr);
        cookie_.pending = std::move(cookie_chunks);
        start_set(/*is_cookie=*/false, /*from_update=*/true);
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
  updating_.store(false);
  if (on_filters_updated_) {
    on_filters_updated_(last_updated_);
  }
}

std::int64_t Ps5Addons::filters_last_updated() const { return last_updated_; }

bool Ps5Addons::filters_updating() const { return updating_.load(); }

}  // namespace prosperoweb::frontend
