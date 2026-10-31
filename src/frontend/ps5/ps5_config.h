// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runtime configuration read from /data/ps5browser/config.json.
#pragma once

#include <string>

namespace ps5browser::core {
class Storage;
}

namespace ps5browser::frontend {

// The default Safari-like user agent for the PS5 build.
inline constexpr const char* kDefaultUserAgent =
    "Mozilla/5.0 (PlayStation; PlayStation 5) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/17.0 Safari/605.1.15 ps5browser/0.1";

struct Ps5Config {
  std::string home_url = "https://duckduckgo.com/";
  std::string user_agent = kDefaultUserAgent;
  std::string language = "en";  // "en" or "ru"
  // Directory where chrome.html and its assets are installed.
  std::string chrome_dir = "/data/ps5browser/chrome";
  // Text entry: "system" (the PS5 keyboard dialog, built-in OSK as the
  // fallback) or "builtin" (the HTML OSK only). PS5BROWSER_OSK overrides it.
  std::string osk = "system";
  int width = 1920;
  int height = 1080;

  // Reads "config" from storage (JSON). Missing or invalid keys keep the
  // defaults. Never throws.
  static Ps5Config load(core::Storage& storage);
};

}  // namespace ps5browser::frontend
