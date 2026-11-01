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

// Translate one Adblock Plus network pattern (the part before any `$`
// options, with `||`, `|`, `^`, `*` anchors) into a WebKit `url-filter`
// regular expression. Exposed for testing.
std::string pattern_to_url_filter(const std::string& pattern);

}  // namespace prosperoweb::core::adblock
