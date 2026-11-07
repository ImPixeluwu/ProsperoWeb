// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <map>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace prosperoweb::core {

// Page zoom remembered per host, as desktop browsers do: zooming a page
// zooms every page of that host, and the level survives restarts (saved by
// Session under kZoomKey). Only levels other than 100% are stored.
class SiteZoom {
 public:
  // The levels zoom_in() / zoom_out() step through.
  static constexpr std::array<double, 13> kLevels = {0.5, 0.67, 0.75, 0.8, 0.9, 1.0, 1.1,
                                                     1.25, 1.5, 1.75, 2.0, 2.5, 3.0};

  // The zoom factor for url's host; 1.0 when none is stored or the URL has
  // no host (about:, data:, file:).
  double for_url(std::string_view url) const;
  // Whether url has a host to remember a zoom level for.
  static bool applies_to(std::string_view url);

  // Step url's host to the next / previous level (from the nearest level when
  // the stored factor is between two) and return the new factor. Clamped at
  // the ends of kLevels. URLs without a host are not zoomed (1.0).
  double zoom_in(std::string_view url);
  double zoom_out(std::string_view url);
  // Back to 100% for url's host.
  double reset(std::string_view url);

  // "125%".
  static std::string percent(double factor);

  std::size_t size() const { return by_host_.size(); }

  nlohmann::json to_json() const;
  // {"host": factor, ...}; entries out of [kLevels.front(), kLevels.back()]
  // or not numbers are skipped. False (and nothing changed) when j is not an
  // object.
  bool from_json(const nlohmann::json& j);

 private:
  double step(std::string_view url, int direction);
  void set(const std::string& host, double factor);

  std::map<std::string, double, std::less<>> by_host_;
};

}  // namespace prosperoweb::core
