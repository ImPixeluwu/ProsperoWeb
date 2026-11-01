// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/adblock.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace prosperoweb::core::adblock {
namespace {

std::string trim(std::string_view s) {
  std::size_t b = 0;
  std::size_t e = s.size();
  while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) {
    ++b;
  }
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) {
    --e;
  }
  return std::string(s.substr(b, e - b));
}

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  out.push_back(cur);
  return out;
}

// WebKit if-domain/unless-domain: a bare domain matches that exact host; a
// leading `*` also matches subdomains. ABP domains cover subdomains, so map
// `example.com` -> `*example.com` (and strip a leading `~`, handled by the
// caller putting it in unless-domain).
std::string web_domain(std::string d) {
  if (!d.empty() && d.front() == '~') {
    d.erase(d.begin());
  }
  if (d.empty()) {
    return d;
  }
  // WebKit rejects the whole list if an if-domain/unless-domain entry is not
  // lowercase ASCII (ContentExtensionError::JSONDomainNotLowerCaseASCII).
  for (char& c : d) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return "*" + d;
}

}  // namespace

std::string pattern_to_url_filter(const std::string& pattern) {
  std::string s = pattern;
  bool domain_anchor = false;
  bool start_anchor = false;
  bool end_anchor = false;
  if (s.rfind("||", 0) == 0) {
    domain_anchor = true;
    s = s.substr(2);
  } else if (!s.empty() && s.front() == '|') {
    start_anchor = true;
    s = s.substr(1);
  }
  if (!s.empty() && s.back() == '|') {
    end_anchor = true;
    s.pop_back();
  }

  std::string body;
  body.reserve(s.size() * 2);
  for (char c : s) {
    switch (c) {
      case '*':
        body += ".*";
        break;
      case '^':
        // ABP separator: any char that is not part of a hostname/path token,
        // or the end of the URL.
        body += "[^a-zA-Z0-9_.%-]";
        break;
      case '.':
      case '+':
      case '?':
      case '$':
      case '{':
      case '}':
      case '(':
      case ')':
      case '|':
      case '[':
      case ']':
      case '\\':
        body += '\\';
        body += c;
        break;
      default:
        body += c;
        break;
    }
  }

  std::string result;
  if (domain_anchor) {
    // Start of URL, optional scheme, optional subdomain labels before the
    // anchored domain.
    result += "^https?://([^/]*\\.)?";
  } else if (start_anchor) {
    result += "^";
  }
  result += body;
  if (end_anchor) {
    result += "$";
  }
  if (result.empty()) {
    result = ".*";
  }
  return result;
}

namespace {

// The filter options this converter understands. Any option outside this set
// (popup, csp, redirect, removeparam, important, badfilter, generichide, ...)
// cannot be expressed as a WebKit rule, so the whole rule is dropped.
bool apply_option(const std::string& opt, nlohmann::json& trigger, bool& ok) {
  auto add = [&](const char* key, const char* value) {
    trigger[key].push_back(value);
  };
  if (opt.empty()) {
    return true;
  }
  if (opt == "third-party") {
    trigger["load-type"] = nlohmann::json::array({"third-party"});
    return true;
  }
  if (opt == "~third-party" || opt == "first-party" || opt == "1p") {
    trigger["load-type"] = nlohmann::json::array({"first-party"});
    return true;
  }
  if (opt == "match-case") {
    trigger["url-filter-is-case-sensitive"] = true;
    return true;
  }
  if (opt.rfind("domain=", 0) == 0) {
    for (const auto& d : split(opt.substr(7), '|')) {
      if (d.empty()) {
        continue;
      }
      if (d.front() == '~') {
        trigger["unless-domain"].push_back(web_domain(d));
      } else {
        trigger["if-domain"].push_back(web_domain(d));
      }
    }
    return true;
  }
  // Resource types (and their ABP negations are not representable per-type
  // in WebKit, so a leading ~ makes the rule unsupported).
  if (!opt.empty() && opt.front() == '~') {
    ok = false;
    return false;
  }
  if (opt == "script") {
    add("resource-type", "script");
    return true;
  }
  if (opt == "image" || opt == "background") {
    add("resource-type", "image");
    return true;
  }
  if (opt == "stylesheet" || opt == "css") {
    add("resource-type", "style-sheet");
    return true;
  }
  if (opt == "font") {
    add("resource-type", "font");
    return true;
  }
  if (opt == "media") {
    add("resource-type", "media");
    return true;
  }
  if (opt == "object" || opt == "object-subrequest") {
    add("resource-type", "raw");
    return true;
  }
  if (opt == "xmlhttprequest" || opt == "xhr" || opt == "other" || opt == "websocket" ||
      opt == "ping" || opt == "beacon") {
    add("resource-type", "raw");
    return true;
  }
  if (opt == "subdocument" || opt == "frame" || opt == "document" || opt == "doc") {
    add("resource-type", "document");
    return true;
  }
  // Unknown or unsupported option.
  ok = false;
  return false;
}

}  // namespace

namespace {

// Parse the lists into separate network and cosmetic rule arrays (no cap), so
// callers can assemble one list or several chunks. Network rules keep input
// order (exceptions follow the blocks they override).
void parse_rules(const std::vector<std::string>& lists, const ConvertOptions& opts,
                 nlohmann::json& network, nlohmann::json& cosmetic, ConvertStats& stats) {
  for (const auto& list : lists) {
    std::istringstream in(list);
    std::string raw;
    while (std::getline(in, raw)) {
      std::string line = trim(raw);
      ++stats.input_lines;
      if (line.empty() || line[0] == '!' || line[0] == '[') {
        continue;
      }
      // Extended-CSS / scriptlet / exception element rules are unsupported.
      if (line.find("#@#") != std::string::npos || line.find("#?#") != std::string::npos ||
          line.find("#$#") != std::string::npos || line.find("#%#") != std::string::npos) {
        ++stats.dropped_unsupported;
        continue;
      }
      if (auto pos = line.find("##"); pos != std::string::npos) {
        if (!opts.include_element_hiding) {
          ++stats.dropped_unsupported;
          continue;
        }
        std::string domains = line.substr(0, pos);
        std::string selector = line.substr(pos + 2);
        if (selector.empty()) {
          ++stats.dropped_unsupported;
          continue;
        }
        nlohmann::json trigger;
        trigger["url-filter"] = ".*";
        if (!domains.empty()) {
          for (const auto& d : split(domains, ',')) {
            if (d.empty()) {
              continue;
            }
            if (d.front() == '~') {
              trigger["unless-domain"].push_back(web_domain(d));
            } else {
              trigger["if-domain"].push_back(web_domain(d));
            }
          }
        }
        nlohmann::json rule;
        rule["trigger"] = std::move(trigger);
        rule["action"] = {{"type", "css-display-none"}, {"selector", selector}};
        cosmetic.push_back(std::move(rule));
        ++stats.element_hide_rules;
        continue;
      }

      bool exception = false;
      if (line.rfind("@@", 0) == 0) {
        exception = true;
        line = line.substr(2);
      }
      std::string pattern = line;
      std::string options;
      const bool is_regex = pattern.size() >= 2 && pattern.front() == '/' &&
                            pattern.find('/', 1) == pattern.size() - 1;
      if (!is_regex) {
        if (auto dollar = pattern.rfind('$'); dollar != std::string::npos) {
          options = pattern.substr(dollar + 1);
          pattern = pattern.substr(0, dollar);
        }
      }
      nlohmann::json trigger;
      bool ok = true;
      if (!options.empty()) {
        for (const auto& opt : split(options, ',')) {
          apply_option(trim(opt), trigger, ok);
          if (!ok) {
            break;
          }
        }
      }
      if (!ok) {
        ++stats.dropped_unsupported;
        continue;
      }
      std::string url_filter;
      if (is_regex) {
        url_filter = pattern.substr(1, pattern.size() - 2);
      } else {
        url_filter = pattern_to_url_filter(pattern);
      }
      if (url_filter.empty()) {
        url_filter = ".*";
      }
      trigger["url-filter"] = url_filter;
      if (!trigger.contains("url-filter-is-case-sensitive")) {
        trigger["url-filter-is-case-sensitive"] = false;
      }
      nlohmann::json rule;
      rule["trigger"] = std::move(trigger);
      rule["action"] = {{"type", exception ? "ignore-previous-rules" : "block"}};
      network.push_back(std::move(rule));
      ++stats.network_rules;
    }
  }
}

}  // namespace

ConvertResult convert(const std::vector<std::string>& lists, const ConvertOptions& opts) {
  ConvertResult result;
  nlohmann::json network = nlohmann::json::array();
  nlohmann::json cosmetic = nlohmann::json::array();
  parse_rules(lists, opts, network, cosmetic, result.stats);

  // Assemble one list, network first so the cap trims cosmetics first.
  nlohmann::json rules = nlohmann::json::array();
  for (auto& r : network) {
    if (rules.size() < opts.max_rules) {
      rules.push_back(std::move(r));
    } else {
      ++result.stats.dropped_cap;
    }
  }
  for (auto& r : cosmetic) {
    if (rules.size() < opts.max_rules) {
      rules.push_back(std::move(r));
    } else {
      ++result.stats.dropped_cap;
    }
  }
  result.stats.emitted = rules.size();
  result.json = rules.dump();
  return result;
}

ChunkedResult convert_chunked(const std::vector<std::string>& lists,
                              std::size_t max_rules_per_chunk, std::size_t max_total_rules,
                              const ConvertOptions& opts) {
  ChunkedResult result;
  if (max_rules_per_chunk == 0) {
    max_rules_per_chunk = 1;
  }
  nlohmann::json network = nlohmann::json::array();
  nlohmann::json cosmetic = nlohmann::json::array();
  parse_rules(lists, opts, network, cosmetic, result.stats);

  // Network first, then cosmetic; split into chunks of <= max_rules_per_chunk,
  // dropping everything past max_total_rules (cosmetic goes first since it
  // trails the network rules).
  nlohmann::json current = nlohmann::json::array();
  std::size_t total = 0;
  auto flush = [&]() {
    if (!current.empty()) {
      result.chunks.push_back(current.dump());
      current = nlohmann::json::array();
    }
  };
  auto emit = [&](nlohmann::json& arr) {
    for (auto& r : arr) {
      if (total >= max_total_rules) {
        ++result.stats.dropped_cap;
        continue;
      }
      current.push_back(std::move(r));
      ++total;
      if (current.size() >= max_rules_per_chunk) {
        flush();
      }
    }
  };
  emit(network);
  emit(cosmetic);
  flush();
  result.stats.emitted = total;
  return result;
}

ConvertResult convert_one(const std::string& list, const ConvertOptions& opts) {
  return convert(std::vector<std::string>{list}, opts);
}

// --- Userscript filter set --------------------------------------------------
namespace {

// Procedural / extended-CSS / scriptlet markers a plain CSS selector cannot
// express; such cosmetic rules are skipped.
bool selector_unsupported(const std::string& sel) {
  if (sel.empty()) {
    return true;
  }
  if (sel.rfind("+js(", 0) == 0) {
    return true;  // scriptlet injection
  }
  static const char* kProc[] = {":has(",     ":has-text(", ":matches-",  ":xpath(",
                                ":nth-ancestor(", ":upward(", ":remove(", ":style(",
                                ":-abp-",    ":watch-attr(", ":min-text-length(",
                                ":others(",  ":if(",     ":if-not(",   ":contains("};
  for (const char* p : kProc) {
    if (sel.find(p) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// A network option set we can approximate by blocking the whole host. Scoping
// or rewriting options (domain=, redirect, csp, removeparam, ...) make a
// whole-host block wrong, so those rules are skipped instead.
bool options_host_safe(const std::string& options) {
  if (options.empty()) {
    return true;
  }
  for (auto& opt : split(options, ',')) {
    std::string o = trim(opt);
    if (!o.empty() && o[0] == '~') {
      o = o.substr(1);
    }
    static const std::unordered_set<std::string> kSafe = {
        "third-party", "first-party", "1p", "3p",   "script",   "image",
        "stylesheet",  "css",         "object", "font",  "media",    "xmlhttprequest",
        "xhr",         "subdocument", "frame",  "document", "doc",   "other",
        "websocket",   "ping",        "beacon", "popup",    "all",   "important",
        "match-case"};
    if (kSafe.find(o) == kSafe.end()) {
      return false;
    }
  }
  return true;
}

// First-party / essential hosts we never put on the block list, so a stray
// list entry cannot break YouTube playback/navigation. (The userscript also
// allowlists these at runtime as defense in depth.)
bool is_first_party_essential(const std::string& host) {
  static const char* kEssential[] = {"youtube.com", "youtube-nocookie.com", "youtubekids.com",
                                      "googlevideo.com", "ytimg.com", "ggpht.com"};
  for (const char* d : kEssential) {
    const std::string dom = d;
    if (host == dom ||
        (host.size() > dom.size() + 1 && host.compare(host.size() - dom.size() - 1,
                                                       dom.size() + 1, "." + dom) == 0)) {
      return true;
    }
  }
  return false;
}

bool is_host_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
         c == '-';
}

std::string lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return s;
}

}  // namespace

std::string FilterSet::to_json() const {
  nlohmann::json out;
  out["hosts"] = hosts;
  std::string css;
  for (const auto& s : selectors) {
    if (!css.empty()) {
      css += ",";
    }
    css += s;
  }
  out["css"] = css.empty() ? std::string() : css + "{display:none!important}";
  nlohmann::json rules = nlohmann::json::array();
  for (const auto& r : domain_rules) {
    rules.push_back(nlohmann::json::array({r.domain, r.selector}));
  }
  out["domainRules"] = std::move(rules);
  return out.dump();
}

FilterSet build_filter_set(const std::vector<std::string>& lists, const FilterSetOptions& opts) {
  FilterSet fs;
  std::unordered_set<std::string> host_seen;
  std::unordered_set<std::string> generic_seen;
  std::size_t generic_css_bytes = 0;

  for (const auto& list : lists) {
    std::istringstream in(list);
    std::string raw;
    while (std::getline(in, raw)) {
      std::string line = trim(raw);
      ++fs.stats.input_lines;
      if (line.empty() || line[0] == '!' || line[0] == '[') {
        continue;
      }

      // Cosmetic rules.
      if (line.find("#@#") != std::string::npos || line.find("#?#") != std::string::npos ||
          line.find("#$#") != std::string::npos || line.find("#%#") != std::string::npos) {
        ++fs.stats.skipped;
        continue;
      }
      if (auto pos = line.find("##"); pos != std::string::npos) {
        std::string domains = line.substr(0, pos);
        std::string selector = line.substr(pos + 2);
        if (selector_unsupported(selector)) {
          ++fs.stats.skipped;
          continue;
        }
        if (domains.empty()) {
          if (generic_css_bytes + selector.size() > opts.max_generic_css_bytes) {
            ++fs.stats.skipped;
            continue;
          }
          if (generic_seen.insert(selector).second) {
            fs.selectors.push_back(selector);
            generic_css_bytes += selector.size() + 1;
            ++fs.stats.selectors;
          }
        } else {
          for (auto& d : split(domains, ',')) {
            std::string dom = trim(d);
            if (dom.empty() || dom[0] == '~') {
              ++fs.stats.skipped;  // negated-domain cosmetic not expressed
              continue;
            }
            if (fs.domain_rules.size() >= opts.max_domain_rules) {
              ++fs.stats.skipped;
              continue;
            }
            fs.domain_rules.push_back({lower(dom), selector});
          }
        }
        continue;
      }

      // Network rules we can turn into a whole-host block.
      if (line.rfind("@@", 0) == 0 || line.rfind("||", 0) != 0) {
        ++fs.stats.skipped;
        continue;
      }
      std::size_t i = 2;
      std::string host;
      while (i < line.size() && is_host_char(line[i])) {
        host.push_back(line[i]);
        ++i;
      }
      const bool whole_host = (i == line.size() || line[i] == '^' || line[i] == '$');
      std::string options;
      if (i < line.size() && line[i] == '^') {
        ++i;
      }
      if (i < line.size() && line[i] == '$') {
        options = line.substr(i + 1);
      } else if (i != line.size()) {
        // Trailing path / wildcard / port: not a whole-host rule.
        ++fs.stats.skipped;
        continue;
      }
      if (!whole_host || host.find('.') == std::string::npos || !options_host_safe(options)) {
        ++fs.stats.skipped;
        continue;
      }
      if (is_first_party_essential(lower(host))) {
        ++fs.stats.skipped;  // never block YouTube/Google media/thumbnails
        continue;
      }
      if (fs.hosts.size() >= opts.max_hosts) {
        ++fs.stats.skipped;
        continue;
      }
      std::string h = lower(host);
      if (host_seen.insert(h).second) {
        fs.hosts.push_back(h);
        ++fs.stats.hosts;
      }
    }
  }
  return fs;
}

}  // namespace prosperoweb::core::adblock
