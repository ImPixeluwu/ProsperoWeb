// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "prosperoweb/core/in_memory_storage.h"
#include "prosperoweb/core/session.h"
#include "prosperoweb/core/site_user_agents.h"

using namespace prosperoweb::core;

TEST_SUITE("site_user_agents") {
  TEST_CASE("presets have ids, labels and strings") {
    for (const auto p : kUaPresets) {
      CAPTURE(ua_preset_id(p));
      CHECK(ua_preset_from_id(ua_preset_id(p)) == p);
      CHECK_FALSE(ua_preset_label(p).empty());
      CHECK(ua_preset_string(p).empty() == (p == UaPreset::Default));
    }
    CHECK_FALSE(ua_preset_from_id("netscape").has_value());
    CHECK(ua_preset_string(UaPreset::Mobile).find("iPhone") != std::string_view::npos);
    CHECK(ua_preset_string(UaPreset::DesktopChrome).find("Chrome/") != std::string_view::npos);
    CHECK(ua_preset_string(UaPreset::SmartTv).find("SMART-TV") != std::string_view::npos);
  }

  TEST_CASE("Default falls back to the configured user agent") {
    SiteUserAgents s;
    CHECK(s.preset_for("https://example.com/") == UaPreset::Default);
    CHECK(s.user_agent_for("https://example.com/", "Configured/1.0") == "Configured/1.0");
  }

  TEST_CASE("per host, case-insensitive, ignoring port and path") {
    SiteUserAgents s;
    REQUIRE(s.set("https://WWW.Example.com:8443/a", UaPreset::Mobile));
    CHECK(s.preset_for("https://www.example.com/b?c") == UaPreset::Mobile);
    CHECK(s.user_agent_for("https://www.example.com/", "X") == ua_preset_string(UaPreset::Mobile));
    CHECK(s.preset_for("https://m.example.com/") == UaPreset::Default);
  }

  TEST_CASE("cycle goes through every preset and back to Default") {
    SiteUserAgents s;
    const char* url = "https://a.example/";
    CHECK(s.cycle(url) == UaPreset::DesktopChrome);
    CHECK(s.cycle(url) == UaPreset::Mobile);
    CHECK(s.cycle(url) == UaPreset::SmartTv);
    CHECK(s.cycle(url) == UaPreset::Default);
    CHECK(s.size() == 0);
  }

  TEST_CASE("URLs without a host have no preset") {
    SiteUserAgents s;
    CHECK_FALSE(SiteUserAgents::applies_to("about:blank"));
    CHECK_FALSE(SiteUserAgents::applies_to("file:///data/x.html"));
    CHECK(SiteUserAgents::applies_to("https://a.example/"));
    CHECK_FALSE(s.set("about:blank", UaPreset::Mobile));
    CHECK(s.cycle("about:blank") == UaPreset::Default);
    CHECK(s.size() == 0);
  }

  TEST_CASE("json round trip skips invalid entries and normalises hosts") {
    SiteUserAgents s;
    CHECK_FALSE(s.from_json(nlohmann::json::array()));
    REQUIRE(s.from_json(nlohmann::json::parse(
        R"({"WWW.A.example.":"mobile","b.example":"warp","c.example":3,"d.example":"default","":"tv"})")));
    CHECK(s.size() == 1);
    CHECK(s.preset_for("https://www.a.example/") == UaPreset::Mobile);
    SiteUserAgents back;
    REQUIRE(back.from_json(s.to_json()));
    CHECK(back.to_json() == nlohmann::json{{"www.a.example", "mobile"}});
  }

  TEST_CASE("saved and loaded by Session") {
    InMemoryStorage storage;
    Session session(storage);
    SiteUserAgents s;
    s.set("https://a.example/", UaPreset::SmartTv);
    REQUIRE(session.save_user_agents(s));
    SiteUserAgents loaded;
    REQUIRE(session.load_user_agents(loaded));
    CHECK(loaded.preset_for("https://a.example/") == UaPreset::SmartTv);
  }
}
