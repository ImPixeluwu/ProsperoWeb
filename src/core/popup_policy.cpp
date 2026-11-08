// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/popup_policy.h"

#include <algorithm>
#include <array>
#include <cctype>

#include "prosperoweb/core/url_normalizer.h"
#include "text_utils.h"

namespace prosperoweb::core {

namespace {

bool is_ipv4_literal(std::string_view host) {
  int dots = 0;
  for (const char c : host) {
    if (c == '.') {
      ++dots;
    } else if (!std::isdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return dots == 3;
}

// Second-level labels that sit under a two-letter country code as part of
// the public suffix (co.uk, com.br, gob.mx, ...). A heuristic, not the PSL.
bool is_common_second_level(std::string_view label) {
  static constexpr std::array<std::string_view, 10> kLabels = {
      "co", "com", "net", "org", "gov", "gob", "edu", "ac", "or", "ne"};
  return std::find(kLabels.begin(), kLabels.end(), label) != kLabels.end();
}

bool has_host(std::string_view url) {
  return url.find("://") != std::string_view::npos;
}

}  // namespace

std::string site_of(std::string_view host) {
  std::string h = detail::to_lower_ascii(detail::trim(host));
  while (!h.empty() && h.back() == '.') {
    h.pop_back();
  }
  if (h.empty() || h.front() == '[' || is_ipv4_literal(h)) {
    return h;
  }
  std::vector<std::string_view> labels;
  std::string_view rest = h;
  for (std::size_t dot = rest.find('.'); dot != std::string_view::npos; dot = rest.find('.')) {
    labels.push_back(rest.substr(0, dot));
    rest.remove_prefix(dot + 1);
  }
  labels.push_back(rest);
  std::size_t keep = 2;
  if (labels.size() >= 3 && labels.back().size() == 2 &&
      is_common_second_level(labels[labels.size() - 2])) {
    keep = 3;
  }
  if (labels.size() <= keep) {
    return h;
  }
  std::string out;
  for (std::size_t i = labels.size() - keep; i < labels.size(); ++i) {
    if (!out.empty()) {
      out += '.';
    }
    out += labels[i];
  }
  return out;
}

std::string site_of_url(std::string_view url) {
  if (!has_host(url)) {
    return {};
  }
  const std::string host = UrlNormalizer::display_host(url);
  // display_host() falls back to the whole URL when there is no authority
  // (file:///...); that is not a site.
  if (host.empty() || host.find_first_of("/:") != std::string::npos) {
    return {};
  }
  return site_of(host);
}

PopupDecision decide_popup(const PopupBlockerConfig& config, const PopupRequest& request) {
  if (!config.enabled) {
    return PopupDecision::Allow;
  }
  const std::string opener_site = site_of_url(request.opener_url);
  if (!opener_site.empty() &&
      std::find(config.allowed_sites.begin(), config.allowed_sites.end(), opener_site) !=
          config.allowed_sites.end()) {
    return PopupDecision::Allow;
  }
  if (!request.user_gesture) {
    return PopupDecision::Block;
  }
  if (request.origin == PopupOrigin::Link || request.origin == PopupOrigin::Form) {
    return PopupDecision::Allow;
  }
  const std::string target_site = site_of_url(request.target_url);
  if (!target_site.empty() && target_site == opener_site) {
    return PopupDecision::Allow;
  }
  return PopupDecision::Block;
}

bool allow_popups_for(PopupBlockerConfig& config, std::string_view url) {
  std::string site = site_of_url(url);
  if (site.empty() ||
      std::find(config.allowed_sites.begin(), config.allowed_sites.end(), site) !=
          config.allowed_sites.end()) {
    return false;
  }
  config.allowed_sites.push_back(std::move(site));
  return true;
}

}  // namespace prosperoweb::core
