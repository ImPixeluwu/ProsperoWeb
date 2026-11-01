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
  std::vector<const char*> positional;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--max-rules") == 0 && i + 1 < argc) {
      opts.max_rules = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--no-element-hiding") == 0) {
      opts.include_element_hiding = false;
    } else {
      positional.push_back(argv[i]);
    }
  }
  if (positional.size() < 2) {
    std::fprintf(stderr,
                 "usage: addons_convert [--max-rules N] [--no-element-hiding] "
                 "<out.json> <in...>\n");
    return 2;
  }

  const char* out_path = positional[0];
  std::vector<std::string> lists;
  for (std::size_t i = 1; i < positional.size(); ++i) {
    lists.push_back(read_file(positional[i]));
  }

  auto result = prosperoweb::core::adblock::convert(lists, opts);

  std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::fprintf(stderr, "addons_convert: cannot write %s\n", out_path);
    return 2;
  }
  out << result.json;
  out.close();

  const auto& s = result.stats;
  std::fprintf(stderr,
               "addons_convert: %s: %zu lines -> %zu rules "
               "(network %zu, element-hide %zu, dropped cap %zu, unsupported %zu)\n",
               out_path, s.input_lines, s.emitted, s.network_rules, s.element_hide_rules,
               s.dropped_cap, s.dropped_unsupported);
  return 0;
}
