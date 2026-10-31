// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace prosperobrowser::core {

enum class Language : std::uint8_t { En, Ru };

// The browser ships no CDM (Widevine, PlayReady, FairPlay). This policy
// decides when to tell the user that protected playback cannot work.
class DrmPolicy {
 public:
  struct EmeRequest {
    std::string origin;
    std::string key_system;
  };

  // Hosts (and their subdomains) whose video is DRM-only.
  static bool is_known_widevine_only(std::string_view host);
  // Any key system other than "org.w3.clearkey" needs a real CDM.
  static bool is_protected_key_system(std::string_view key_system);
  static std::string_view notice_text(Language lang);

  // True the first time it is called for an origin in this session.
  bool should_notify(std::string_view origin);

  // Called from the EME detector page script. Records the request and
  // returns true if the frontend should show the notice now (protected key
  // system and first time for this origin).
  bool record_eme_request(std::string_view origin, std::string_view key_system);

  const std::vector<EmeRequest>& eme_requests() const { return eme_requests_; }

 private:
  std::set<std::string, std::less<>> notified_origins_;
  std::vector<EmeRequest> eme_requests_;
};

}  // namespace prosperobrowser::core
