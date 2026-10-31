// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "prosperoweb/core/tab_manager.h"

using prosperoweb::core::TabManager;

TEST_SUITE("tab_manager") {
  TEST_CASE("open makes the new tab active and ids are unique") {
    TabManager tm;
    CHECK(tm.empty());
    CHECK(tm.active() == nullptr);
    const auto a = tm.open("https://a.example");
    const auto b = tm.open("https://b.example");
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    CHECK(*a != *b);
    CHECK(tm.size() == 2);
    REQUIRE(tm.active() != nullptr);
    CHECK(tm.active()->id == *b);
    CHECK(tm.active()->url == "https://b.example");
  }

  TEST_CASE("limit of kMaxTabs") {
    TabManager tm;
    for (std::size_t i = 0; i < TabManager::kMaxTabs; ++i) {
      CHECK(tm.open("about:blank").has_value());
    }
    CHECK(tm.full());
    CHECK_FALSE(tm.open("about:blank").has_value());
    CHECK(tm.size() == TabManager::kMaxTabs);
  }

  TEST_CASE("closing the active tab activates the right neighbour, else the left") {
    TabManager tm;
    const auto a = *tm.open("a");
    const auto b = *tm.open("b");
    const auto c = *tm.open("c");
    tm.activate(b);
    CHECK(tm.close(b));
    CHECK(tm.active()->id == c);  // right neighbour
    CHECK(tm.close(c));
    CHECK(tm.active()->id == a);  // no right neighbour -> left
    CHECK(tm.close(a));
    CHECK(tm.empty());
    CHECK(tm.active() == nullptr);
    CHECK_FALSE(tm.close(a));
  }

  TEST_CASE("closing an inactive tab keeps the active one") {
    TabManager tm;
    const auto a = *tm.open("a");
    const auto b = *tm.open("b");
    CHECK(tm.close(a));
    CHECK(tm.active()->id == b);
  }

  TEST_CASE("ids are never reused") {
    TabManager tm;
    const auto a = *tm.open("a");
    tm.close(a);
    const auto b = *tm.open("b");
    CHECK(b != a);
  }

  TEST_CASE("activate, next and prev are cyclic") {
    TabManager tm;
    const auto a = *tm.open("a");
    const auto b = *tm.open("b");
    const auto c = *tm.open("c");
    CHECK_FALSE(tm.activate(999));
    CHECK(tm.activate(a));
    tm.next();
    CHECK(tm.active()->id == b);
    tm.next();
    CHECK(tm.active()->id == c);
    tm.next();
    CHECK(tm.active()->id == a);
    tm.prev();
    CHECK(tm.active()->id == c);
    TabManager empty;
    empty.next();
    empty.prev();
    CHECK(empty.active() == nullptr);
  }

  TEST_CASE("updates") {
    TabManager tm;
    const auto a = *tm.open("a");
    CHECK(tm.update_title(a, "Title"));
    CHECK(tm.update_url(a, "https://new"));
    CHECK(tm.set_loading(a, true));
    CHECK(tm.set_nav_state(a, true, false));
    const auto* t = tm.find(a);
    REQUIRE(t != nullptr);
    CHECK(t->title == "Title");
    CHECK(t->url == "https://new");
    CHECK(t->loading);
    CHECK(t->can_go_back);
    CHECK_FALSE(t->can_go_forward);
    CHECK_FALSE(tm.update_title(12345, "x"));
  }

  TEST_CASE("session roundtrip reassigns ids and keeps the active index") {
    TabManager tm;
    tm.open("https://one");
    const auto two = *tm.open("https://two");
    tm.open("https://three");
    tm.update_title(two, "Two");
    tm.activate(two);
    const auto j = tm.to_json();

    TabManager restored;
    restored.open("pre-existing");
    REQUIRE(restored.from_json(j));
    REQUIRE(restored.size() == 3);
    CHECK(restored.tabs()[1].url == "https://two");
    CHECK(restored.tabs()[1].title == "Two");
    CHECK(restored.active()->url == "https://two");
    // ids continue monotonically after the pre-existing tab (id 1)
    CHECK(restored.tabs()[0].id == 2);
    CHECK(restored.tabs()[2].id == 4);

    CHECK_FALSE(restored.from_json(nlohmann::json::array()));
    CHECK(restored.size() == 3);
  }
}
