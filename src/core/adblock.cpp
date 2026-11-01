// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/adblock.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>
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

ConvertResult convert(const std::vector<std::string>& lists, const ConvertOptions& opts) {
  ConvertResult result;
  nlohmann::json rules = nlohmann::json::array();
  bool capped = false;

  auto at_cap = [&]() { return rules.size() >= opts.max_rules; };

  for (const auto& list : lists) {
    std::istringstream in(list);
    std::string raw;
    while (std::getline(in, raw)) {
      std::string line = trim(raw);
      ++result.stats.input_lines;
      if (line.empty() || line[0] == '!' || line[0] == '[') {
        continue;
      }

      // --- Element hiding -------------------------------------------------
      // Extended-CSS / scriptlet / exception element rules are not supported
      // by WebKit's css-display-none action.
      if (line.find("#@#") != std::string::npos || line.find("#?#") != std::string::npos ||
          line.find("#$#") != std::string::npos || line.find("#%#") != std::string::npos) {
        ++result.stats.dropped_unsupported;
        continue;
      }
      if (auto pos = line.find("##"); pos != std::string::npos) {
        if (!opts.include_element_hiding) {
          ++result.stats.dropped_unsupported;
          continue;
        }
        std::string domains = line.substr(0, pos);
        std::string selector = line.substr(pos + 2);
        if (selector.empty()) {
          ++result.stats.dropped_unsupported;
          continue;
        }
        if (capped || at_cap()) {
          capped = true;
          ++result.stats.dropped_cap;
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
        rules.push_back(std::move(rule));
        ++result.stats.element_hide_rules;
        continue;
      }

      // --- Network rules --------------------------------------------------
      bool exception = false;
      if (line.rfind("@@", 0) == 0) {
        exception = true;
        line = line.substr(2);
      }

      std::string pattern = line;
      std::string options;
      // Options follow the last `$`, unless the pattern is a literal /regex/.
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
        ++result.stats.dropped_unsupported;
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
        // ABP matching is case-insensitive; WebKit defaults to sensitive.
        trigger["url-filter-is-case-sensitive"] = false;
      }

      if (capped || at_cap()) {
        capped = true;
        ++result.stats.dropped_cap;
        continue;
      }
      nlohmann::json rule;
      rule["trigger"] = std::move(trigger);
      rule["action"] = {{"type", exception ? "ignore-previous-rules" : "block"}};
      rules.push_back(std::move(rule));
      ++result.stats.network_rules;
    }
  }

  result.stats.emitted = rules.size();
  result.json = rules.dump();
  return result;
}

ConvertResult convert_one(const std::string& list, const ConvertOptions& opts) {
  return convert(std::vector<std::string>{list}, opts);
}

}  // namespace prosperoweb::core::adblock
