// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <string_view>

namespace prosperoweb::core {

// Turns address-bar input into a loadable URL.
class UrlNormalizer {
 public:
  static constexpr std::string_view kDefaultSearchTemplate = "https://duckduckgo.com/?q={}";

  // Rules, in order:
  //  - input is trimmed; empty input yields "about:blank";
  //  - "about:", "file:" and any "<scheme>://" URL pass through unchanged;
  //  - "localhost[:port][/...]" and IPv4 literals get "http://";
  //  - anything else containing a dot and no whitespace gets "https://";
  //  - otherwise the input is percent-encoded (UTF-8, RFC 3986 unreserved
  //    characters kept, space as %20) and substituted for the first "{}" in
  //    search_template (appended if the template has no placeholder).
  static std::string normalize(std::string_view input,
                               std::string_view search_template = kDefaultSearchTemplate);

  // Host of a hierarchical URL, lower-cased, without userinfo and port.
  // Returns the input unchanged when there is no host (about:blank, file:///).
  static std::string display_host(std::string_view url);

  // Percent-encodes everything except RFC 3986 unreserved characters.
  static std::string percent_encode(std::string_view s);
};

}  // namespace prosperoweb::core
