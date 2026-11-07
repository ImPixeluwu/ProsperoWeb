// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/drm_policy.h"

#include <array>

#include "text_utils.h"

namespace prosperoweb::core {

namespace {

constexpr std::array<std::string_view, 14> kWidevineOnlyHosts = {
    "kinopoisk.ru", "hd.kinopoisk.ru", "okko.tv",      "start.ru",       "kion.ru",
    "wink.ru",      "amediateka.ru",   "netflix.com",  "disneyplus.com", "primevideo.com",
    "max.com",      "tv.apple.com",    "crunchyroll.com", "tubitv.com",
};

bool host_matches(std::string_view host, std::string_view domain) {
  if (host == domain) {
    return true;
  }
  return host.size() > domain.size() && host.ends_with(domain) &&
         host[host.size() - domain.size() - 1] == '.';
}

}  // namespace

bool DrmPolicy::is_known_widevine_only(std::string_view host) {
  std::string h = detail::to_lower_ascii(detail::trim(host));
  while (!h.empty() && h.back() == '.') {
    h.pop_back();
  }
  if (h.empty()) {
    return false;
  }
  for (const auto domain : kWidevineOnlyHosts) {
    if (host_matches(h, domain)) {
      return true;
    }
  }
  return false;
}

bool DrmPolicy::is_protected_key_system(std::string_view key_system) {
  return key_system != "org.w3.clearkey";
}

std::string_view DrmPolicy::notice_text(Language lang) { return tr(lang, Text::DrmText); }

bool DrmPolicy::should_notify(std::string_view origin) {
  return notified_origins_.insert(detail::to_lower_ascii(detail::trim(origin))).second;
}

bool DrmPolicy::record_eme_request(std::string_view origin, std::string_view key_system) {
  eme_requests_.push_back(EmeRequest{std::string(origin), std::string(key_system)});
  if (!is_protected_key_system(key_system)) {
    return false;
  }
  return should_notify(origin);
}

}  // namespace prosperoweb::core
