// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <string>

#include "ps5browser/core/drm_policy.h"
#include "ps5browser/core/page_scripts.h"

using namespace ps5browser::core;

TEST_SUITE("drm_policy") {
  TEST_CASE("known DRM-only hosts including subdomains") {
    for (const char* host : {"kinopoisk.ru", "hd.kinopoisk.ru", "okko.tv", "start.ru", "kion.ru",
                             "wink.ru", "amediateka.ru", "netflix.com", "www.netflix.com",
                             "disneyplus.com", "primevideo.com", "play.max.com", "tv.apple.com",
                             "crunchyroll.com", "tubitv.com", "WWW.OKKO.TV."}) {
      CAPTURE(host);
      CHECK(DrmPolicy::is_known_widevine_only(host));
    }
    CHECK_FALSE(DrmPolicy::is_known_widevine_only("youtube.com"));
    CHECK_FALSE(DrmPolicy::is_known_widevine_only("notnetflix.com"));
    CHECK_FALSE(DrmPolicy::is_known_widevine_only("apple.com"));
    CHECK_FALSE(DrmPolicy::is_known_widevine_only("netflix.com.evil.test"));
    CHECK_FALSE(DrmPolicy::is_known_widevine_only(""));
  }

  TEST_CASE("notice texts") {
    CHECK(DrmPolicy::notice_text(Language::En).starts_with("This site requested protected (DRM)"));
    CHECK(DrmPolicy::notice_text(Language::Ru).starts_with("Этот сайт запросил"));
    CHECK(DrmPolicy::notice_text(Language::En).find("Widevine, PlayReady or FairPlay") !=
          std::string_view::npos);
  }

  TEST_CASE("notify once per origin") {
    DrmPolicy p;
    CHECK(p.should_notify("https://www.netflix.com"));
    CHECK_FALSE(p.should_notify("https://www.netflix.com"));
    CHECK_FALSE(p.should_notify("HTTPS://WWW.NETFLIX.COM"));
    CHECK(p.should_notify("https://hd.kinopoisk.ru"));
  }

  TEST_CASE("eme requests") {
    DrmPolicy p;
    CHECK_FALSE(p.record_eme_request("https://demo.test", "org.w3.clearkey"));
    CHECK(p.record_eme_request("https://demo.test", "com.widevine.alpha"));
    CHECK_FALSE(p.record_eme_request("https://demo.test", "com.microsoft.playready"));
    CHECK(p.eme_requests().size() == 3);
    CHECK(p.eme_requests()[1].key_system == "com.widevine.alpha");
    CHECK(DrmPolicy::is_protected_key_system("com.apple.fps"));
    CHECK_FALSE(DrmPolicy::is_protected_key_system("org.w3.clearkey"));
  }

  TEST_CASE("page scripts are present") {
    const std::string eme = kEmeDetectorScript;
    CHECK(eme.find("requestMediaKeySystemAccess") != std::string::npos);
    CHECK(eme.find("messageHandlers") != std::string::npos);
    CHECK(eme.find("NotSupportedError") != std::string::npos);
    CHECK(std::string(kGamepadKeyBridgeScript).find("TODO") != std::string::npos);
  }
}
