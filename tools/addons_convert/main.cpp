// SPDX-License-Identifier: GPL-3.0-or-later
//
// Build-time wrapper around prosperoweb::core::adblock::convert. The staging
// script (scripts/ps5/addons/stage-addons.sh) runs this to turn downloaded
// EasyList/EasyPrivacy/EasyList-Cookie text into the WebKit content-rule-list
// JSON that is bundled with the browser. The on-device updater
// (ps5_addons.cpp) calls the same convert() so a bundled ruleset and a
// console-recompiled one are byte-identical for identical input.
//
//   addons_convert [--max-rules N] [--no-element-hiding] <out.json> <in...>
//
// Writes the JSON to <out.json> and a one-line stats summary to stderr.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "prosperoweb/core/adblock.h"

namespace {

std::string read_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "addons_convert: cannot read %s\n", path);
    std::exit(2);
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
  prosperoweb::core::adblock::ConvertOptions opts;
  bool filter_set = false;
  std::vector<const char*> positional;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--max-rules") == 0 && i + 1 < argc) {
      opts.max_rules = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--no-element-hiding") == 0) {
      opts.include_element_hiding = false;
    } else if (std::strcmp(argv[i], "--filter-set") == 0) {
      // Produce the userscript filter set (hosts + cosmetic CSS + domain
      // rules) instead of the WebKit content-rule-list JSON. This is what the
      // console uses (ENABLE(CONTENT_EXTENSIONS) is off in our build).
      filter_set = true;
    } else {
      positional.push_back(argv[i]);
    }
  }
  if (positional.size() < 2) {
    std::fprintf(stderr,
                 "usage: addons_convert [--filter-set] [--max-rules N] "
                 "[--no-element-hiding] <out.json> <in...>\n");
    return 2;
  }

  const char* out_path = positional[0];
  std::vector<std::string> lists;
  for (std::size_t i = 1; i < positional.size(); ++i) {
    // Per-list stats (so the staging log shows per-list counts).
    const std::string text = read_file(positional[i]);
    if (filter_set) {
      auto fs = prosperoweb::core::adblock::build_filter_set({text});
      std::fprintf(stderr,
                   "addons_convert: %s: %zu lines -> %zu hosts, %zu generic, %zu domain, "
                   "%zu skipped\n",
                   positional[i], fs.stats.input_lines, fs.stats.hosts, fs.stats.selectors,
                   fs.domain_rules.size(), fs.stats.skipped);
    } else {
      auto r = prosperoweb::core::adblock::convert_one(text, opts);
      std::fprintf(stderr,
                   "addons_convert: %s: %zu lines -> %zu rules (dropped cap %zu, unsupported %zu)\n",
                   positional[i], r.stats.input_lines, r.stats.emitted, r.stats.dropped_cap,
                   r.stats.dropped_unsupported);
    }
    lists.push_back(text);
  }

  std::string json;
  if (filter_set) {
    auto fs = prosperoweb::core::adblock::build_filter_set(lists);
    json = fs.to_json();
    std::fprintf(stderr,
                 "addons_convert: %s (merged): %zu hosts, %zu generic selectors, %zu domain rules\n",
                 out_path, fs.hosts.size(), fs.selectors.size(), fs.domain_rules.size());
  } else {
    auto result = prosperoweb::core::adblock::convert(lists, opts);
    json = result.json;
    std::fprintf(stderr, "addons_convert: %s (merged): %zu rules\n", out_path, result.stats.emitted);
  }

  std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::fprintf(stderr, "addons_convert: cannot write %s\n", out_path);
    return 2;
  }
  out << json;
  return 0;
}
