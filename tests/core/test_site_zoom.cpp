// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "prosperoweb/core/in_memory_storage.h"
#include "prosperoweb/core/session.h"
#include "prosperoweb/core/site_zoom.h"

using namespace prosperoweb::core;

TEST_SUITE("site_zoom") {
  TEST_CASE("100% by default and for URLs without a host") {
    SiteZoom z;
    CHECK(z.for_url("https://example.com/") == doctest::Approx(1.0));
    CHECK(z.zoom_in("about:blank") == doctest::Approx(1.0));
    CHECK(z.zoom_in("file:///data/x.html") == doctest::Approx(1.0));
    CHECK(z.size() == 0);
    CHECK(SiteZoom::applies_to("https://example.com/"));
    CHECK_FALSE(SiteZoom::applies_to("about:blank"));
    CHECK_FALSE(SiteZoom::applies_to("file:///data/x.html"));
  }

  TEST_CASE("steps through the levels per host") {
    SiteZoom z;
    CHECK(z.zoom_in("https://news.example/a") == doctest::Approx(1.1));
    CHECK(z.zoom_in("https://news.example/b") == doctest::Approx(1.25));
    CHECK(z.for_url("https://news.example/other?q=1") == doctest::Approx(1.25));
    CHECK(z.for_url("https://other.example/") == doctest::Approx(1.0));
    CHECK(z.zoom_out("https://news.example/") == doctest::Approx(1.1));
    CHECK(z.zoom_out("https://other.example/") == doctest::Approx(0.9));
    CHECK(z.size() == 2);
  }

  TEST_CASE("the host is case-insensitive and ignores port and trailing dot") {
    SiteZoom z;
    z.zoom_in("https://News.Example:8443/");
    CHECK(z.for_url("https://news.example./x") == doctest::Approx(1.1));
  }

  TEST_CASE("clamped at the ends") {
    SiteZoom z;
    for (int i = 0; i < 20; ++i) z.zoom_in("https://a.example/");
    CHECK(z.for_url("https://a.example/") == doctest::Approx(SiteZoom::kLevels.back()));
    for (int i = 0; i < 20; ++i) z.zoom_out("https://a.example/");
    CHECK(z.for_url("https://a.example/") == doctest::Approx(SiteZoom::kLevels.front()));
  }

  TEST_CASE("back to 100% forgets the host") {
    SiteZoom z;
    z.zoom_in("https://a.example/");
    z.zoom_out("https://a.example/");
    CHECK(z.size() == 0);
    z.zoom_in("https://a.example/");
    CHECK(z.reset("https://a.example/") == doctest::Approx(1.0));
    CHECK(z.size() == 0);
  }

  TEST_CASE("a stored factor between two levels steps to the neighbour") {
    SiteZoom z;
    REQUIRE(z.from_json(nlohmann::json{{"a.example", 1.3}}));
    CHECK(z.zoom_in("https://a.example/") == doctest::Approx(1.5));
    REQUIRE(z.from_json(nlohmann::json{{"a.example", 1.3}}));
    CHECK(z.zoom_out("https://a.example/") == doctest::Approx(1.25));
  }

  TEST_CASE("percent labels") {
    CHECK(SiteZoom::percent(1.0) == "100%");
    CHECK(SiteZoom::percent(1.25) == "125%");
    CHECK(SiteZoom::percent(0.67) == "67%");
  }

  TEST_CASE("json round trip skips invalid entries") {
    SiteZoom z;
    CHECK_FALSE(z.from_json(nlohmann::json::array()));
    REQUIRE(z.from_json(nlohmann::json::parse(
        R"({"a.example":1.5,"b.example":"big","c.example":9,"d.example":1.0,"":2})")));
    CHECK(z.size() == 1);
    CHECK(z.for_url("https://a.example/") == doctest::Approx(1.5));
    SiteZoom back;
    REQUIRE(back.from_json(z.to_json()));
    CHECK(back.for_url("https://a.example/") == doctest::Approx(1.5));
  }

  TEST_CASE("saved and loaded by Session") {
    InMemoryStorage storage;
    Session session(storage);
    SiteZoom z;
    z.zoom_in("https://a.example/");
    REQUIRE(session.save_zoom(z));
    SiteZoom loaded;
    REQUIRE(session.load_zoom(loaded));
    CHECK(loaded.for_url("https://a.example/") == doctest::Approx(1.1));
  }
}
