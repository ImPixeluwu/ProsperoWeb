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
  bool chunked = false;
  std::size_t per_chunk = 20000;
  std::size_t max_total = 100000;
  std::vector<const char*> positional;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--max-rules") == 0 && i + 1 < argc) {
      opts.max_rules = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--no-element-hiding") == 0) {
      opts.include_element_hiding = false;
    } else if (std::strcmp(argv[i], "--chunked") == 0) {
      // Produce several small content rule lists (<prefix>-N.json) instead of
      // one big one, so WebKit's on-device compiler does not trap/OOM.
      chunked = true;
    } else if (std::strcmp(argv[i], "--per-chunk") == 0 && i + 1 < argc) {
      per_chunk = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (std::strcmp(argv[i], "--max-total") == 0 && i + 1 < argc) {
      max_total = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else {
      positional.push_back(argv[i]);
    }
  }
  if (positional.size() < 2) {
    std::fprintf(stderr,
                 "usage: addons_convert [--chunked --per-chunk N --max-total M] "
                 "[--max-rules N] [--no-element-hiding] <out.json|prefix> <in...>\n");
    return 2;
  }

  const char* out_path = positional[0];
  std::vector<std::string> lists;
  for (std::size_t i = 1; i < positional.size(); ++i) {
    const std::string text = read_file(positional[i]);
    auto r = prosperoweb::core::adblock::convert_one(text, opts);
    std::fprintf(stderr,
                 "addons_convert: %s: %zu lines -> %zu rules (network %zu, element-hide %zu, "
                 "unsupported %zu)\n",
                 positional[i], r.stats.input_lines, r.stats.emitted, r.stats.network_rules,
                 r.stats.element_hide_rules, r.stats.dropped_unsupported);
    lists.push_back(text);
  }

  if (chunked) {
    // out_path is a prefix: strip a trailing ".json" and write <prefix>-N.json.
    std::string prefix = out_path;
    if (prefix.size() > 5 && prefix.compare(prefix.size() - 5, 5, ".json") == 0) {
      prefix.resize(prefix.size() - 5);
    }
    auto res = prosperoweb::core::adblock::convert_chunked(lists, per_chunk, max_total, opts);
    for (std::size_t i = 0; i < res.chunks.size(); ++i) {
      const std::string path = prefix + "-" + std::to_string(i) + ".json";
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      if (!out) {
        std::fprintf(stderr, "addons_convert: cannot write %s\n", path.c_str());
        return 2;
      }
      out << res.chunks[i];
      std::fprintf(stderr, "addons_convert: %s: %zu bytes\n", path.c_str(), res.chunks[i].size());
    }
    std::fprintf(stderr,
                 "addons_convert: %s: %zu chunks, %zu rules emitted (per-chunk %zu, total cap %zu, "
                 "dropped cap %zu)\n",
                 prefix.c_str(), res.chunks.size(), res.stats.emitted, per_chunk, max_total,
                 res.stats.dropped_cap);
    std::printf("%zu\n", res.chunks.size());  // chunk count on stdout for the stager
    return 0;
  }

  auto result = prosperoweb::core::adblock::convert(lists, opts);
  std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::fprintf(stderr, "addons_convert: cannot write %s\n", out_path);
    return 2;
  }
  out << result.json;
  std::fprintf(stderr, "addons_convert: %s (merged): %zu rules\n", out_path, result.stats.emitted);
  return 0;
}
