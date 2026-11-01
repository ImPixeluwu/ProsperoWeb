// SPDX-License-Identifier: GPL-3.0-or-later
//
// Converts Adblock Plus / EasyList filter-list text into the JSON a WebKit
// content rule list (WKUserContentExtensionStore) compiles. This is the ONE
// converter implementation in the project: the build-time staging tool
// (tools/addons_convert) and the on-device updater (ps5_addons.cpp) both call
// it, so a bundled ruleset and one recompiled on the console come out of the
// same code.
//
// It supports a practical subset of the filter syntax (network blocking and
// exceptions, basic element hiding); extended-CSS and scriptlet rules are
// dropped and counted. WebKit's rule engine does not do uBlock-style cosmetic
// filtering, so coverage is close to Safari content blockers, not uBlock
// Origin. See docs and NOTES.txt for the honest limitations.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace prosperoweb::core::adblock {

struct ConvertOptions {
  // WebKit's content-extension parser rejects a list over maxRuleCount =
  // 150000 rules (WebCore ContentExtensionParser.cpp). Rules past this cap
  // are dropped (counted in ConvertStats::dropped_cap) rather than failing
  // the whole compile.
  std::size_t max_rules = 150000;
  // Emit css-display-none rules for `##selector` element-hiding filters.
  bool include_element_hiding = true;
};

struct ConvertStats {
  std::size_t input_lines = 0;
  std::size_t network_rules = 0;     // block / ignore-previous-rules emitted
  std::size_t element_hide_rules = 0;
  std::size_t emitted = 0;           // total rules in the JSON
  std::size_t dropped_cap = 0;       // dropped because over max_rules
  std::size_t dropped_unsupported = 0;
};

struct ConvertResult {
  std::string json;  // a JSON array, ready for WKUserContentExtensionStoreCompile
  ConvertStats stats;
};

// Convert one filter list.
ConvertResult convert_one(const std::string& list, const ConvertOptions& opts = {});

// Convert several lists into one combined rule list (EasyList + EasyPrivacy,
// etc.). Rules keep the input order across lists so exception rules
// (@@, ignore-previous-rules) still follow the block rules they override.
ConvertResult convert(const std::vector<std::string>& lists,
                      const ConvertOptions& opts = {});

// Split the converted rules into SEVERAL content rule lists, each no larger
// than max_rules_per_chunk, capped at max_total_rules overall. WebKit's
// on-device content-extension compiler asserts / OOMs on a very large single
// list, so the ad blocker ships multiple small compiled lists (like wBlock's
// several content blockers). Network rules are emitted first (so the cap trims
// cosmetics first); each chunk is a standalone JSON array ready to compile.
struct ChunkedResult {
  std::vector<std::string> chunks;  // each a JSON array
  ConvertStats stats;
};
ChunkedResult convert_chunked(const std::vector<std::string>& lists,
                              std::size_t max_rules_per_chunk, std::size_t max_total_rules,
                              const ConvertOptions& opts = {});

// Translate one Adblock Plus network pattern (the part before any `$`
// options, with `||`, `|`, `^`, `*` anchors) into a WebKit `url-filter`
// regular expression. Exposed for testing.
std::string pattern_to_url_filter(const std::string& pattern);

// --- Userscript filter set --------------------------------------------------
//
// Our WebKit build has ENABLE(CONTENT_EXTENSIONS) off, so the rule-list API is
// unusable on the console. Instead we extract, from the same filter lists, a
// compact set an injected userscript can enforce: generic element-hiding CSS
// selectors (cosmetic) and whole-host block entries (||host^ rules, the clean
// ad/tracker servers). Path/regex/option-heavy network rules and
// domain-scoped or procedural cosmetic rules are left out and counted.

struct FilterSetOptions {
  // Separate budgets so one kind cannot starve another (e.g. EasyList's large
  // generic cosmetic list must not crowd out uBlock's YouTube domain rules).
  std::size_t max_hosts = 50000;                 // whole-host network blocks
  std::size_t max_generic_css_bytes = 192 * 1024;  // generic element-hiding CSS
  std::size_t max_domain_rules = 20000;          // domain-scoped cosmetic rules
};

struct FilterSetStats {
  std::size_t input_lines = 0;
  std::size_t hosts = 0;
  std::size_t selectors = 0;
  std::size_t skipped = 0;  // rules not expressible this way
};

struct DomainRule {
  std::string domain;    // e.g. "youtube.com" (matches it and its subdomains)
  std::string selector;  // CSS selector to hide on that domain
};

struct FilterSet {
  std::vector<std::string> hosts;        // whole-host blocks, e.g. "doubleclick.net"
  std::vector<std::string> selectors;    // generic (all-site) element-hiding selectors
  std::vector<DomainRule> domain_rules;  // domain-scoped cosmetic (e.g. YouTube page ads)
  FilterSetStats stats;

  // Serialise as {"hosts":[...],"css":"<generic>{display:none!important}",
  // "domainRules":[["domain","selector"],...]} for the adblock userscript.
  std::string to_json() const;
};

FilterSet build_filter_set(const std::vector<std::string>& lists,
                           const FilterSetOptions& opts = {});

}  // namespace prosperoweb::core::adblock
