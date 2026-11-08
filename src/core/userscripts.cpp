// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/userscripts.h"

#include <algorithm>
#include <cctype>
#include <regex>

#include "text_utils.h"

namespace prosperoweb::core {

namespace {

constexpr std::string_view kMetaStart = "==UserScript==";
constexpr std::string_view kMetaEnd = "==/UserScript==";

std::string escape_regex_char(char c) {
  static constexpr std::string_view kSpecial = "\\^$.|?*+()[]{}/";
  if (kSpecial.find(c) != std::string_view::npos) {
    return std::string("\\") + c;
  }
  return std::string(1, c);
}

std::string escape_regex(std::string_view s) {
  std::string out;
  for (const char c : s) out += escape_regex_char(c);
  return out;
}

// Glob to regex body: "*" is any run of characters, everything else literal.
std::string glob_body(std::string_view glob) {
  std::string out;
  for (const char c : glob) out += c == '*' ? std::string(".*") : escape_regex_char(c);
  return out;
}

std::string without_fragment(std::string_view url) {
  return std::string(url.substr(0, url.find('#')));
}

bool regex_matches(const std::string& source, const std::string& text) {
  if (source.empty()) {
    return false;
  }
  try {
    return std::regex_search(text, std::regex(source, std::regex::ECMAScript | std::regex::icase));
  } catch (const std::regex_error&) {
    return false;  // a broken /regex/ from the script never matches
  }
}

std::vector<std::string> include_regexes(const UserscriptMeta& m) {
  std::vector<std::string> out;
  for (const auto& p : m.matches) {
    if (auto r = match_pattern_to_regex(p); !r.empty()) out.push_back(std::move(r));
  }
  for (const auto& g : m.includes) {
    if (auto r = include_glob_to_regex(g); !r.empty()) out.push_back(std::move(r));
  }
  return out;
}

std::vector<std::string> exclude_regexes(const UserscriptMeta& m) {
  std::vector<std::string> out;
  for (const auto& p : m.exclude_matches) {
    if (auto r = match_pattern_to_regex(p); !r.empty()) out.push_back(std::move(r));
  }
  for (const auto& g : m.excludes) {
    if (auto r = include_glob_to_regex(g); !r.empty()) out.push_back(std::move(r));
  }
  return out;
}

constexpr const char* kAnyWeb = "^https?://";

}  // namespace

std::string UserscriptMeta::id() const { return namespace_ + "/" + name; }

std::optional<UserscriptMeta> parse_userscript_meta(std::string_view source) {
  const auto start = source.find(kMetaStart);
  if (start == std::string_view::npos) {
    return std::nullopt;
  }
  const auto end = source.find(kMetaEnd, start);
  if (end == std::string_view::npos) {
    return std::nullopt;
  }
  UserscriptMeta meta;
  std::string_view block = source.substr(start + kMetaStart.size(), end - start - kMetaStart.size());
  while (!block.empty()) {
    const auto nl = block.find('\n');
    std::string_view line = detail::trim(block.substr(0, nl));
    block = nl == std::string_view::npos ? std::string_view{} : block.substr(nl + 1);
    if (!line.starts_with("//")) {
      continue;
    }
    line = detail::trim(line.substr(2));
    if (!line.starts_with("@")) {
      continue;
    }
    const auto space = line.find_first_of(" \t");
    const std::string key(line.substr(1, space == std::string_view::npos ? line.size() - 1 : space - 1));
    const std::string value(space == std::string_view::npos ? std::string_view{}
                                                            : detail::trim(line.substr(space)));
    if (key == "name" && meta.name.empty()) meta.name = value;
    else if (key == "namespace") meta.namespace_ = value;
    else if (key == "version") meta.version = value;
    else if (key == "description" && meta.description.empty()) meta.description = value;
    else if (key == "match" && !value.empty()) meta.matches.push_back(value);
    else if (key == "include" && !value.empty()) meta.includes.push_back(value);
    else if (key == "exclude" && !value.empty()) meta.excludes.push_back(value);
    else if (key == "exclude-match" && !value.empty()) meta.exclude_matches.push_back(value);
    else if (key == "grant" && !value.empty()) meta.grants.push_back(value);
    else if (key == "noframes") meta.noframes = true;
    else if (key == "run-at") {
      if (value == "document-start") meta.run_at = RunAt::DocumentStart;
      else if (value == "document-idle") meta.run_at = RunAt::DocumentIdle;
      else meta.run_at = RunAt::DocumentEnd;
    }
  }
  if (meta.name.empty()) {
    return std::nullopt;
  }
  return meta;
}

std::string match_pattern_to_regex(std::string_view pattern) {
  pattern = detail::trim(pattern);
  if (pattern == "<all_urls>") {
    return "^(?:https?|file|ftp)://";
  }
  const auto sep = pattern.find("://");
  if (sep == std::string_view::npos) {
    return {};
  }
  const std::string_view scheme = pattern.substr(0, sep);
  std::string out = "^";
  if (scheme == "*") out += "https?";
  else if (scheme == "http" || scheme == "https" || scheme == "file" || scheme == "ftp") out += scheme;
  else return {};
  out += "://";
  const std::string_view rest = pattern.substr(sep + 3);
  const auto slash = rest.find('/');
  if (slash == std::string_view::npos) {
    return {};  // a match pattern always has a path
  }
  const std::string_view host = rest.substr(0, slash);
  if (host == "*") {
    out += "[^/]*";
  } else if (host.starts_with("*.")) {
    if (host.find('*', 1) != std::string_view::npos) return {};
    out += "(?:[^/]*\\.)?" + escape_regex(host.substr(2));
  } else if (host.find('*') != std::string_view::npos) {
    return {};
  } else if (host.empty() && scheme != "file") {
    return {};
  } else {
    out += escape_regex(host);
  }
  if (!host.empty()) {
    out += "(?::\\d+)?";
  }
  out += glob_body(rest.substr(slash)) + "$";
  return out;
}

std::string include_glob_to_regex(std::string_view glob) {
  glob = detail::trim(glob);
  if (glob.empty()) {
    return {};
  }
  if (glob.size() >= 2 && glob.front() == '/' && glob.back() == '/') {
    return std::string(glob.substr(1, glob.size() - 2));
  }
  return "^" + glob_body(glob) + "$";
}

bool userscript_applies(const UserscriptMeta& meta, std::string_view url) {
  const std::string href = without_fragment(url);
  const auto includes = include_regexes(meta);
  bool included = false;
  if (meta.matches.empty() && meta.includes.empty()) {
    included = regex_matches(kAnyWeb, href);
  } else {
    included = std::any_of(includes.begin(), includes.end(),
                           [&](const std::string& r) { return regex_matches(r, href); });
  }
  if (!included) {
    return false;
  }
  const auto excludes = exclude_regexes(meta);
  return std::none_of(excludes.begin(), excludes.end(),
                      [&](const std::string& r) { return regex_matches(r, href); });
}

std::string userscript_wrapper(const UserscriptMeta& meta, std::string_view source) {
  const bool any_web = meta.matches.empty() && meta.includes.empty();
  nlohmann::json inc = any_web ? std::vector<std::string>{kAnyWeb} : include_regexes(meta);
  nlohmann::json exc = exclude_regexes(meta);
  nlohmann::json info = {{"name", meta.name},
                         {"namespace", meta.namespace_},
                         {"version", meta.version},
                         {"description", meta.description}};
  const auto dump = [](const nlohmann::json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
  };
  const char* run_at = meta.run_at == RunAt::DocumentStart ? "start"
                       : meta.run_at == RunAt::DocumentIdle ? "idle"
                                                            : "end";
  std::string js;
  js.reserve(source.size() + 4096);
  js += "(function () {\n'use strict';\n";
  js += "var ID = " + dump(meta.id()) + ";\n";
  js += "var INC = " + dump(inc) + ", EXC = " + dump(exc) + ";\n";
  js += "var NOFRAMES = " + std::string(meta.noframes ? "true" : "false") + ";\n";
  js += "var RUN_AT = '" + std::string(run_at) + "';\n";
  js += "var INFO = " + dump(info) + ";\n";
  js += R"JS(var href = location.href.split('#')[0];
function test(list) {
  for (var i = 0; i < list.length; i++) {
    try { if (new RegExp(list[i], 'i').test(href)) return true; } catch (e) {}
  }
  return false;
}
if (!test(INC) || test(EXC)) return;
if (NOFRAMES && window.top !== window.self) return;
var seen = window.__prosperowebUserscripts = window.__prosperowebUserscripts || {};
if (seen[ID]) return;
seen[ID] = true;
var GM_info = { script: INFO, scriptHandler: 'ProsperoWeb', version: '1' };
var PREFIX = '__prosperoweb_us:' + ID + ':';
function GM_getValue(k, d) {
  try { var v = localStorage.getItem(PREFIX + k); return v === null ? d : JSON.parse(v); }
  catch (e) { return d; }
}
function GM_setValue(k, v) { try { localStorage.setItem(PREFIX + k, JSON.stringify(v)); } catch (e) {} }
function GM_deleteValue(k) { try { localStorage.removeItem(PREFIX + k); } catch (e) {} }
function GM_listValues() {
  var out = [];
  try {
    for (var i = 0; i < localStorage.length; i++) {
      var k = localStorage.key(i);
      if (k && k.indexOf(PREFIX) === 0) out.push(k.slice(PREFIX.length));
    }
  } catch (e) {}
  return out;
}
function GM_addStyle(css) {
  var s = document.createElement('style');
  s.textContent = css;
  (document.head || document.documentElement).appendChild(s);
  return s;
}
function GM_log() { console.log.apply(console, arguments); }
function GM_openInTab(url) { return window.open(url, '_blank'); }
function GM_xmlhttpRequest(d) {
  var init = { method: d.method || 'GET', headers: d.headers || {}, body: d.data };
  fetch(d.url, init).then(function (r) {
    return r.text().then(function (t) {
      var res = { status: r.status, statusText: r.statusText, responseText: t, response: t,
                  finalUrl: r.url, readyState: 4, responseHeaders: '' };
      if (d.onload) d.onload(res);
      if (d.onloadend) d.onloadend(res);
    });
  }).catch(function (e) { if (d.onerror) d.onerror(e); if (d.onloadend) d.onloadend(e); });
  return { abort: function () {} };
}
var GM = {
  info: GM_info,
  getValue: function (k, d) { return Promise.resolve(GM_getValue(k, d)); },
  setValue: function (k, v) { return Promise.resolve(GM_setValue(k, v)); },
  deleteValue: function (k) { return Promise.resolve(GM_deleteValue(k)); },
  listValues: function () { return Promise.resolve(GM_listValues()); },
  addStyle: function (c) { return Promise.resolve(GM_addStyle(c)); },
  openInTab: GM_openInTab,
  xmlHttpRequest: GM_xmlhttpRequest
};
var unsafeWindow = window;
function run() {
  try {
    (function () {
)JS";
  js += source;
  // A trailing line comment in the source must not swallow the closing code.
  js += R"JS(
    }).call(window);
  } catch (e) { console.error('[userscript ' + INFO.name + ']', e); }
}
if (RUN_AT === 'start') {
  run();
} else if (document.readyState === 'loading') {
  document.addEventListener('DOMContentLoaded', function () {
    if (RUN_AT === 'idle') setTimeout(run, 0); else run();
  }, { once: true });
} else {
  run();
}
})();
)JS";
  return js;
}

std::optional<std::size_t> UserscriptStore::find(const UserscriptMeta& meta) const {
  const std::string id = meta.id();
  for (std::size_t i = 0; i < scripts_.size(); ++i) {
    if (scripts_[i].meta.id() == id) return i;
  }
  return std::nullopt;
}

std::optional<std::size_t> UserscriptStore::install(std::string source) {
  if (source.size() > kMaxUserscriptBytes) {
    return std::nullopt;
  }
  auto meta = parse_userscript_meta(source);
  if (!meta) {
    return std::nullopt;
  }
  if (const auto existing = find(*meta)) {
    scripts_[*existing].source = std::move(source);
    scripts_[*existing].meta = std::move(*meta);
    return existing;
  }
  scripts_.push_back({std::move(source), true, std::move(*meta)});
  return scripts_.size() - 1;
}

bool UserscriptStore::remove(std::size_t index) {
  if (index >= scripts_.size()) return false;
  scripts_.erase(scripts_.begin() + static_cast<std::ptrdiff_t>(index));
  return true;
}

bool UserscriptStore::set_enabled(std::size_t index, bool enabled) {
  if (index >= scripts_.size()) return false;
  scripts_[index].enabled = enabled;
  return true;
}

std::vector<std::string> UserscriptStore::injected_sources() const {
  std::vector<std::string> out;
  for (const auto& s : scripts_) {
    if (s.enabled) out.push_back(userscript_wrapper(s.meta, s.source));
  }
  return out;
}

std::size_t UserscriptStore::count_for_url(std::string_view url) const {
  return static_cast<std::size_t>(std::count_if(scripts_.begin(), scripts_.end(), [&](const Entry& s) {
    return s.enabled && userscript_applies(s.meta, url);
  }));
}

nlohmann::json UserscriptStore::to_json() const {
  nlohmann::json out = nlohmann::json::array();
  for (const auto& s : scripts_) {
    out.push_back({{"source", s.source}, {"enabled", s.enabled}});
  }
  return out;
}

bool UserscriptStore::from_json(const nlohmann::json& j) {
  if (!j.is_array()) {
    return false;
  }
  scripts_.clear();
  for (const auto& item : j) {
    if (!item.is_object() || !item.contains("source") || !item["source"].is_string()) {
      continue;
    }
    if (const auto i = install(item["source"].get<std::string>())) {
      if (const auto e = item.find("enabled"); e != item.end() && e->is_boolean()) {
        scripts_[*i].enabled = e->get<bool>();
      }
    }
  }
  return true;
}

bool is_userscript_url(std::string_view url) {
  if (!detail::starts_with_ci(url, "http://") && !detail::starts_with_ci(url, "https://")) {
    return false;
  }
  std::string_view path = url.substr(0, url.find_first_of("?#"));
  return path.size() > 8 && detail::to_lower_ascii(path.substr(path.size() - 8)) == ".user.js";
}

std::string userscript_source_script() {
  return R"JS((function () {
  'use strict';
  var url = location.href;
  function post(text) {
    try {
      window.webkit.messageHandlers.prosperoweb.postMessage(
          { type: 'userscript_source', url: url, source: text });
    } catch (e) {
    }
  }
  function fromDocument() {
    return document.documentElement ? document.documentElement.textContent : '';
  }
  fetch(url).then(function (r) { return r.text(); }).then(post, function () { post(fromDocument()); });
})();)JS";
}

}  // namespace prosperoweb::core
