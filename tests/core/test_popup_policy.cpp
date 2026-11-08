// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "prosperoweb/core/addons_config.h"
#include "prosperoweb/core/popup_policy.h"

using namespace prosperoweb::core;

namespace {

PopupRequest request(const char* opener, const char* target, bool gesture, PopupOrigin origin) {
  return PopupRequest{opener, target, gesture, origin};
}

}  // namespace

TEST_SUITE("popup_policy") {
  TEST_CASE("site_of approximates the registrable domain") {
    CHECK(site_of("www.example.com") == "example.com");
    CHECK(site_of("m.youtube.com") == "youtube.com");
    CHECK(site_of("example.com") == "example.com");
    CHECK(site_of("WWW.Example.COM.") == "example.com");
    CHECK(site_of("news.bbc.co.uk") == "bbc.co.uk");
    CHECK(site_of("a.b.example.com.br") == "example.com.br");
    CHECK(site_of("bbc.co.uk") == "bbc.co.uk");
    CHECK(site_of("192.168.1.10") == "192.168.1.10");
    CHECK(site_of("[::1]") == "[::1]");
    CHECK(site_of("localhost") == "localhost");
    CHECK(site_of("") == "");
  }

  TEST_CASE("site_of_url") {
    CHECK(site_of_url("https://www.example.com:8443/a?b#c") == "example.com");
    CHECK(site_of_url("https://user:pw@sub.example.org/") == "example.org");
    CHECK(site_of_url("about:blank") == "");
    CHECK(site_of_url("") == "");
    CHECK(site_of_url("file:///data/prosperoweb/x.html") == "");
  }

  TEST_CASE("pop-ups without a user gesture are blocked") {
    PopupBlockerConfig cfg;
    for (auto origin : {PopupOrigin::Link, PopupOrigin::Form, PopupOrigin::Script}) {
      CAPTURE(static_cast<int>(origin));
      CHECK(decide_popup(cfg, request("https://site.test/", "https://site.test/x", false, origin)) ==
            PopupDecision::Block);
    }
  }

  TEST_CASE("links and forms the user activated open, even to other sites") {
    PopupBlockerConfig cfg;
    CHECK(decide_popup(cfg, request("https://news.test/", "https://other.test/", true,
                                    PopupOrigin::Link)) == PopupDecision::Allow);
    CHECK(decide_popup(cfg, request("https://news.test/", "https://pay.test/", true,
                                    PopupOrigin::Form)) == PopupDecision::Allow);
  }

  TEST_CASE("window.open from a click: same site opens, another site is a pop-under") {
    PopupBlockerConfig cfg;
    CHECK(decide_popup(cfg, request("https://www.video.test/watch", "https://player.video.test/1",
                                    true, PopupOrigin::Script)) == PopupDecision::Allow);
    CHECK(decide_popup(cfg, request("https://video.test/watch", "https://ads.example/landing", true,
                                    PopupOrigin::Script)) == PopupDecision::Block);
    CHECK(decide_popup(cfg, request("https://video.test/watch", "about:blank", true,
                                    PopupOrigin::Script)) == PopupDecision::Block);
    CHECK(decide_popup(cfg, request("https://video.test/watch", "", true, PopupOrigin::Script)) ==
          PopupDecision::Block);
  }

  TEST_CASE("allowed sites and the off switch allow everything from the opener") {
    PopupBlockerConfig cfg;
    cfg.allowed_sites = {"bank.test"};
    CHECK(decide_popup(cfg, request("https://login.bank.test/", "https://ads.example/", false,
                                    PopupOrigin::Script)) == PopupDecision::Allow);
    CHECK(decide_popup(cfg, request("https://other.test/", "https://ads.example/", false,
                                    PopupOrigin::Script)) == PopupDecision::Block);
    cfg.enabled = false;
    CHECK(decide_popup(cfg, request("https://other.test/", "https://ads.example/", false,
                                    PopupOrigin::Script)) == PopupDecision::Allow);
  }

  TEST_CASE("allow_popups_for adds the opener's site once") {
    PopupBlockerConfig cfg;
    CHECK(allow_popups_for(cfg, "https://www.bank.test/login"));
    CHECK_FALSE(allow_popups_for(cfg, "https://m.bank.test/"));
    CHECK_FALSE(allow_popups_for(cfg, "about:blank"));
    REQUIRE(cfg.allowed_sites.size() == 1);
    CHECK(cfg.allowed_sites[0] == "bank.test");
  }

  TEST_CASE("persisted in the add-ons config") {
    auto defaults = AddonsConfig::from_json(nlohmann::json::object());
    CHECK(defaults.popups.enabled);
    CHECK(defaults.popups.allowed_sites.empty());

    auto parsed = AddonsConfig::from_json(nlohmann::json::parse(
        R"({"popups":{"enabled":false,"allowed_sites":["www.Bank.test.","bank.test",7,""]}})"));
    CHECK_FALSE(parsed.popups.enabled);
    REQUIRE(parsed.popups.allowed_sites.size() == 1);
    CHECK(parsed.popups.allowed_sites[0] == "bank.test");

    auto round = AddonsConfig::from_json(parsed.to_json());
    CHECK_FALSE(round.popups.enabled);
    CHECK(round.popups.allowed_sites == parsed.popups.allowed_sites);
  }
}
