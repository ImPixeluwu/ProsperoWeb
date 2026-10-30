// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <string>

#include "ps5browser/core/history.h"

using ps5browser::core::History;

TEST_SUITE("history") {
  TEST_CASE("duplicates are merged") {
    History h;
    h.add("https://a", "A", 100);
    h.add("https://b", "B", 110);
    h.add("https://a", "A2", 120);
    h.add("https://a", "", 130);
    REQUIRE(h.size() == 2);
    const auto r = h.recent(10);
    CHECK(r[0].url == "https://a");
    CHECK(r[0].visit_count == 3);
    CHECK(r[0].visited_at == 130);
    CHECK(r[0].title == "A2");  // empty title does not overwrite
    CHECK(r[1].url == "https://b");
    CHECK(h.recent(1).size() == 1);
  }

  TEST_CASE("cap evicts the oldest entry") {
    History h;
    for (std::size_t i = 0; i < History::kMaxEntries; ++i) {
      h.add("https://site/" + std::to_string(i), "", static_cast<std::int64_t>(1000 + i));
    }
    h.add("https://site/0", "", 5000);  // refresh the oldest one
    h.add("https://new", "", 6000);
    CHECK(h.size() == History::kMaxEntries);
    // site/1 was the oldest after site/0 was refreshed.
    const auto all = h.recent(History::kMaxEntries);
    bool has_site1 = false;
    bool has_site0 = false;
    for (const auto& e : all) {
      has_site1 = has_site1 || e.url == "https://site/1";
      has_site0 = has_site0 || e.url == "https://site/0";
    }
    CHECK_FALSE(has_site1);
    CHECK(has_site0);
  }

  TEST_CASE("search is case-insensitive and ordered by count then time") {
    History h;
    h.add("https://news.example/one", "Morning News", 100);
    h.add("https://example.org", "Example", 200);
    h.add("https://example.org", "Example", 210);
    h.add("https://docs.example/news", "Docs", 300);
    h.add("https://other.test", "Новости дня", 400);

    const auto r = h.search("NEWS", 10);
    REQUIRE(r.size() == 2);
    CHECK(r[0].url == "https://docs.example/news");  // same count, newer
    CHECK(r[1].url == "https://news.example/one");

    const auto ex = h.search("example", 10);
    REQUIRE(ex.size() == 3);
    CHECK(ex[0].url == "https://example.org");  // highest visit_count

    CHECK(h.search("новости", 10).size() == 1);
    CHECK(h.search("example", 1).size() == 1);
    CHECK(h.search("nothing-matches", 10).empty());
  }

  TEST_CASE("json roundtrip") {
    History h;
    h.add("https://a", "A", 1);
    h.add("https://a", "A", 2);
    h.add("https://b", "B", 3);
    History r;
    REQUIRE(r.from_json(h.to_json()));
    REQUIRE(r.size() == 2);
    CHECK(r.recent(1)[0].url == "https://b");
    CHECK(r.search("https://a", 1)[0].visit_count == 2);
    CHECK_FALSE(r.from_json(nlohmann::json::object()));
    CHECK(r.remove("https://a"));
    CHECK(r.size() == 1);
  }
}
