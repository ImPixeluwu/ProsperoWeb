// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "ps5browser/core/bookmarks.h"

using ps5browser::core::Bookmarks;

TEST_SUITE("bookmarks") {
  TEST_CASE("add, dedupe, remove") {
    Bookmarks b;
    CHECK(b.add("https://a", "A", 10));
    CHECK_FALSE(b.add("https://a", "A again", 20));
    CHECK(b.add("https://b", "B", 30));
    CHECK(b.size() == 2);
    CHECK(b.contains("https://a"));
    CHECK(b.list()[0].title == "A");
    CHECK(b.remove("https://a"));
    CHECK_FALSE(b.remove("https://a"));
    CHECK_FALSE(b.contains("https://a"));
    CHECK(b.list().size() == 1);
  }

  TEST_CASE("json roundtrip skips invalid and duplicate items") {
    Bookmarks b;
    b.add("https://a", "A", 10);
    b.add("https://b", "B", 20);
    auto j = b.to_json();
    j.push_back({{"title", "no url"}});
    j.push_back({{"url", "https://a"}, {"title", "dup"}});
    Bookmarks r;
    REQUIRE(r.from_json(j));
    REQUIRE(r.size() == 2);
    CHECK(r.list()[1].url == "https://b");
    CHECK(r.list()[1].added_at == 20);
    CHECK_FALSE(r.from_json("not an array"));
  }
}
