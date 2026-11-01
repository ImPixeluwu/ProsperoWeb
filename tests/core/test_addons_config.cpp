// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "prosperoweb/core/addons_config.h"
#include "prosperoweb/core/filter_update.h"

using prosperoweb::core::AddonsConfig;
using prosperoweb::core::should_refresh_filters;

TEST_SUITE("addons_config") {
  TEST_CASE("defaults when the key is absent (migration)") {
    auto root = nlohmann::json::parse(R"({"home_url":"https://x"})");
    auto cfg = AddonsConfig::from_config_root(root);
    CHECK(cfg.adblock.enabled == true);
    CHECK(cfg.adblock.cookie_banners == false);
    CHECK(cfg.adblock.auto_update == true);
    CHECK(cfg.adblock.last_updated == 0);
    CHECK(cfg.sponsorblock.enabled == false);
    REQUIRE(cfg.sponsorblock.categories.size() == 1);
    CHECK(cfg.sponsorblock.categories[0] == "sponsor");
  }

  TEST_CASE("partial objects keep per-key defaults") {
    auto addons = nlohmann::json::parse(R"({"adblock":{"cookie_banners":true}})");
    auto cfg = AddonsConfig::from_json(addons);
    CHECK(cfg.adblock.enabled == true);          // default kept
    CHECK(cfg.adblock.cookie_banners == true);   // overridden
    CHECK(cfg.sponsorblock.categories[0] == "sponsor");
  }

  TEST_CASE("full round trip") {
    AddonsConfig cfg;
    cfg.adblock.enabled = false;
    cfg.adblock.cookie_banners = true;
    cfg.adblock.auto_update = false;
    cfg.adblock.last_updated = 1700000000;
    cfg.sponsorblock.enabled = true;
    cfg.sponsorblock.categories = {"sponsor", "intro", "music_offtopic"};
    auto back = AddonsConfig::from_json(cfg.to_json());
    CHECK(back.adblock.enabled == false);
    CHECK(back.adblock.cookie_banners == true);
    CHECK(back.adblock.auto_update == false);
    CHECK(back.adblock.last_updated == 1700000000);
    CHECK(back.sponsorblock.enabled == true);
    CHECK(back.sponsorblock.categories ==
          std::vector<std::string>{"sponsor", "intro", "music_offtopic"});
  }

  TEST_CASE("unknown and duplicate categories are filtered") {
    auto addons = nlohmann::json::parse(
        R"({"sponsorblock":{"categories":["sponsor","bogus","sponsor","filler"]}})");
    auto cfg = AddonsConfig::from_json(addons);
    CHECK(cfg.sponsorblock.categories == std::vector<std::string>{"sponsor", "filler"});
  }

  TEST_CASE("the eight standard categories are offered") {
    CHECK(AddonsConfig::all_sponsorblock_categories().size() == 8);
    CHECK(AddonsConfig::is_sponsorblock_category("music_offtopic"));
    CHECK_FALSE(AddonsConfig::is_sponsorblock_category("nonsense"));
  }
}

TEST_SUITE("filter_update") {
  TEST_CASE("never updated -> refresh when enabled") {
    CHECK(should_refresh_filters(/*adblock=*/true, /*auto=*/true, /*last=*/0, /*now=*/1000));
  }

  TEST_CASE("disabled ad blocker or auto-update -> no refresh") {
    CHECK_FALSE(should_refresh_filters(false, true, 0, 1000));
    CHECK_FALSE(should_refresh_filters(true, false, 0, 1000));
  }

  TEST_CASE("age threshold (24h default)") {
    const std::int64_t day = 24 * 60 * 60;
    std::int64_t now = 10 * day;
    CHECK_FALSE(should_refresh_filters(true, true, now - (day - 1), now));  // fresh
    CHECK(should_refresh_filters(true, true, now - day, now));              // exactly stale
    CHECK(should_refresh_filters(true, true, now - 2 * day, now));          // old
  }

  TEST_CASE("future timestamp (clock skew) is treated as fresh") {
    CHECK_FALSE(should_refresh_filters(true, true, 5000, 1000));
  }

  TEST_CASE("custom threshold") {
    CHECK(should_refresh_filters(true, true, 0, 1000, 60));
    CHECK_FALSE(should_refresh_filters(true, true, 950, 1000, 60));
    CHECK(should_refresh_filters(true, true, 900, 1000, 60));
  }
}
