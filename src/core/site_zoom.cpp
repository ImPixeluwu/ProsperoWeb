// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/site_zoom.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "prosperoweb/core/url_normalizer.h"

namespace prosperoweb::core {

namespace {

// The URL's host, or "" when it has none (about:, data:, file:///...).
std::string host_of(std::string_view url) {
  if (url.find("://") == std::string_view::npos) {
    return {};
  }
  std::string host = UrlNormalizer::display_host(url);
  // display_host() falls back to the whole URL when there is no authority.
  if (host.empty() || host.find_first_of("/:") != std::string::npos) {
    return {};
  }
  while (!host.empty() && host.back() == '.') {
    host.pop_back();
  }
  return host;
}

bool is_default(double factor) { return std::fabs(factor - 1.0) < 1e-6; }

// Index of the level nearest to factor.
std::size_t nearest_level(double factor) {
  std::size_t best = 0;
  for (std::size_t i = 1; i < SiteZoom::kLevels.size(); ++i) {
    if (std::fabs(SiteZoom::kLevels[i] - factor) < std::fabs(SiteZoom::kLevels[best] - factor)) {
      best = i;
    }
  }
  return best;
}

}  // namespace

bool SiteZoom::applies_to(std::string_view url) { return !host_of(url).empty(); }

double SiteZoom::for_url(std::string_view url) const {
  const std::string host = host_of(url);
  if (host.empty()) {
    return 1.0;
  }
  const auto it = by_host_.find(host);
  return it == by_host_.end() ? 1.0 : it->second;
}

double SiteZoom::step(std::string_view url, int direction) {
  const std::string host = host_of(url);
  if (host.empty()) {
    return 1.0;
  }
  const double current = for_url(url);
  std::size_t i = nearest_level(current);
  // From between two levels, the first step lands on the neighbour in the
  // requested direction rather than skipping it.
  if (direction > 0 && kLevels[i] > current + 1e-6) {
    direction = 0;
  } else if (direction < 0 && kLevels[i] < current - 1e-6) {
    direction = 0;
  }
  if (direction > 0 && i + 1 < kLevels.size()) {
    ++i;
  } else if (direction < 0 && i > 0) {
    --i;
  }
  set(host, kLevels[i]);
  return kLevels[i];
}

double SiteZoom::zoom_in(std::string_view url) { return step(url, +1); }
double SiteZoom::zoom_out(std::string_view url) { return step(url, -1); }

double SiteZoom::reset(std::string_view url) {
  const std::string host = host_of(url);
  if (!host.empty()) {
    set(host, 1.0);
  }
  return 1.0;
}

void SiteZoom::set(const std::string& host, double factor) {
  if (is_default(factor)) {
    by_host_.erase(host);
  } else {
    by_host_[host] = factor;
  }
}

std::string SiteZoom::percent(double factor) {
  return std::to_string(static_cast<int>(std::lround(factor * 100.0))) + "%";
}

nlohmann::json SiteZoom::to_json() const {
  nlohmann::json out = nlohmann::json::object();
  for (const auto& [host, factor] : by_host_) {
    out[host] = factor;
  }
  return out;
}

bool SiteZoom::from_json(const nlohmann::json& j) {
  if (!j.is_object()) {
    return false;
  }
  by_host_.clear();
  for (const auto& [host, value] : j.items()) {
    if (host.empty() || !value.is_number()) {
      continue;
    }
    const double factor = value.get<double>();
    if (factor < kLevels.front() - 1e-6 || factor > kLevels.back() + 1e-6) {
      continue;
    }
    set(host, factor);
  }
  return true;
}

}  // namespace prosperoweb::core
