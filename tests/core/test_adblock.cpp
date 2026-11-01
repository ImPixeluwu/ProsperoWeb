// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "prosperoweb/core/adblock.h"

using namespace prosperoweb::core::adblock;

TEST_SUITE("adblock") {
  TEST_CASE("pattern to url-filter: domain anchor and separator") {
    CHECK(pattern_to_url_filter("||ads.example.com^") ==
          "^https?://([^/]*\\.)?ads\\.example\\.com[^a-zA-Z0-9_.%-]");
    CHECK(pattern_to_url_filter("||example.com/ads/*") ==
          "^https?://([^/]*\\.)?example\\.com/ads/.*");
  }

  TEST_CASE("pattern to url-filter: start/end anchors and wildcards") {
    CHECK(pattern_to_url_filter("|http://bad.example|") == "^http://bad\\.example$");
    CHECK(pattern_to_url_filter("/banner/*/ad") == "/banner/.*/ad");
    CHECK(pattern_to_url_filter("") == ".*");
  }

  TEST_CASE("a simple block rule becomes a block action") {
    auto r = convert_one("||ads.example.com^");
    auto doc = nlohmann::json::parse(r.json);
    REQUIRE(doc.is_array());
    REQUIRE(doc.size() == 1);
    CHECK(doc[0]["action"]["type"] == "block");
    CHECK(doc[0]["trigger"]["url-filter"] ==
          "^https?://([^/]*\\.)?ads\\.example\\.com[^a-zA-Z0-9_.%-]");
    // ABP is case-insensitive, WebKit defaults to sensitive.
    CHECK(doc[0]["trigger"]["url-filter-is-case-sensitive"] == false);
    CHECK(r.stats.network_rules == 1);
  }

  TEST_CASE("options map to resource-type, load-type and domains") {
    auto r = convert_one("||track.example^$third-party,script,image,domain=a.com|~b.com");
    auto doc = nlohmann::json::parse(r.json);
    REQUIRE(doc.size() == 1);
    const auto& trig = doc[0]["trigger"];
    CHECK(trig["load-type"] == nlohmann::json::array({"third-party"}));
    CHECK(trig["resource-type"] == nlohmann::json::array({"script", "image"}));
    CHECK(trig["if-domain"] == nlohmann::json::array({"*a.com"}));
    CHECK(trig["unless-domain"] == nlohmann::json::array({"*b.com"}));
  }

  TEST_CASE("exception rules become ignore-previous-rules") {
    auto r = convert_one("@@||example.com/allow^");
    auto doc = nlohmann::json::parse(r.json);
    REQUIRE(doc.size() == 1);
    CHECK(doc[0]["action"]["type"] == "ignore-previous-rules");
  }

  TEST_CASE("element hiding becomes css-display-none") {
    SUBCASE("global") {
      auto r = convert_one("##.ad-banner");
      auto doc = nlohmann::json::parse(r.json);
      REQUIRE(doc.size() == 1);
      CHECK(doc[0]["action"]["type"] == "css-display-none");
      CHECK(doc[0]["action"]["selector"] == ".ad-banner");
      CHECK(doc[0]["trigger"]["url-filter"] == ".*");
      CHECK(r.stats.element_hide_rules == 1);
    }
    SUBCASE("domain-scoped") {
      auto r = convert_one("example.com,~sub.example.com##div#ad");
      auto doc = nlohmann::json::parse(r.json);
      REQUIRE(doc.size() == 1);
      CHECK(doc[0]["trigger"]["if-domain"] == nlohmann::json::array({"*example.com"}));
      CHECK(doc[0]["trigger"]["unless-domain"] ==
            nlohmann::json::array({"*sub.example.com"}));
    }
  }

  TEST_CASE("comments, headers and blank lines are skipped") {
    auto r = convert_one("[Adblock Plus 2.0]\n! a comment\n\n||keep.example^\n");
    CHECK(r.stats.emitted == 1);
    CHECK(r.stats.network_rules == 1);
  }

  TEST_CASE("unsupported syntax is dropped and counted") {
    // Extended-CSS, scriptlet and exception element rules; an unsupported
    // network option (popup).
    auto r = convert_one(
        "example.com#?#div:has(> .ad)\n"
        "example.com#@#.whitelisted\n"
        "||example.com^$popup\n");
    CHECK(r.stats.emitted == 0);
    CHECK(r.stats.dropped_unsupported == 3);
  }

  TEST_CASE("the rule cap drops the overflow and reports it") {
    std::string list;
    for (int i = 0; i < 10; ++i) {
      list += "||host" + std::to_string(i) + ".example^\n";
    }
    ConvertOptions opts;
    opts.max_rules = 4;
    auto r = convert_one(list, opts);
    auto doc = nlohmann::json::parse(r.json);
    CHECK(doc.size() == 4);
    CHECK(r.stats.emitted == 4);
    CHECK(r.stats.dropped_cap == 6);
  }

  TEST_CASE("filter set: hosts, generic and domain cosmetic, skips") {
    std::string list =
        "! comment\n"
        "||ads.example.com^\n"              // whole-host block
        "||track.example^$third-party\n"     // safe option -> host
        "||scoped.example^$domain=foo.com\n" // scoping option -> skipped
        "||example.com/ads.js\n"             // path -> skipped (not whole-host)
        "##.generic-ad\n"                     // generic cosmetic
        "youtube.com,m.youtube.com##ytd-promoted-video-renderer\n"  // domain cosmetic
        "example.com##div:has(> .ad)\n"      // procedural -> skipped
        "example.com##+js(nano-sib)\n"        // scriptlet -> skipped
        "@@||allow.example^\n";               // exception -> skipped
    auto fs = prosperoweb::core::adblock::build_filter_set({list});
    CHECK(fs.hosts == std::vector<std::string>{"ads.example.com", "track.example"});
    CHECK(fs.selectors == std::vector<std::string>{".generic-ad"});
    REQUIRE(fs.domain_rules.size() == 2);
    CHECK(fs.domain_rules[0].domain == "youtube.com");
    CHECK(fs.domain_rules[0].selector == "ytd-promoted-video-renderer");
    CHECK(fs.domain_rules[1].domain == "m.youtube.com");
    CHECK(fs.stats.hosts == 2);
    CHECK(fs.stats.selectors == 1);
    CHECK(fs.stats.skipped >= 5);

    auto doc = nlohmann::json::parse(fs.to_json());
    CHECK(doc["css"] == ".generic-ad{display:none!important}");
    CHECK(doc["hosts"].size() == 2);
    CHECK(doc["domainRules"][0][0] == "youtube.com");
  }

  TEST_CASE("filter set: caps bound the payload") {
    std::string list;
    for (int i = 0; i < 100; ++i) {
      list += "||host" + std::to_string(i) + ".example^\n";
    }
    prosperoweb::core::adblock::FilterSetOptions o;
    o.max_hosts = 10;
    auto fs = prosperoweb::core::adblock::build_filter_set({list}, o);
    CHECK(fs.hosts.size() == 10);
    CHECK(fs.stats.skipped == 90);
  }

  TEST_CASE("multiple lists keep input order (golden smoke)") {
    std::vector<std::string> lists = {"||ads.example^", "##.promo"};
    auto r = convert(lists);
    CHECK(r.json ==
          "[{\"action\":{\"type\":\"block\"},\"trigger\":{\"url-filter\":"
          "\"^https?://([^/]*\\\\.)?ads\\\\.example[^a-zA-Z0-9_.%-]\","
          "\"url-filter-is-case-sensitive\":false}},"
          "{\"action\":{\"selector\":\".promo\",\"type\":\"css-display-none\"},"
          "\"trigger\":{\"url-filter\":\".*\"}}]");
  }
}
