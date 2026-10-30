// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <stdlib.h>
#include <unistd.h>

#include <cstdio>
#include <string>

#include "ps5browser/core/file_storage.h"
#include "ps5browser/core/in_memory_storage.h"
#include "ps5browser/core/session.h"

using namespace ps5browser::core;

namespace {

void fill(TabManager& tabs, History& history, Bookmarks& bookmarks) {
  tabs.open("https://one.example");
  const auto two = *tabs.open("https://two.example");
  tabs.update_title(two, "Two");
  history.add("https://one.example", "One", 100);
  history.add("https://two.example", "Two", 200);
  bookmarks.add("https://two.example", "Two", 300);
}

void check_restored(Storage& storage) {
  Session session(storage);
  TabManager tabs;
  History history;
  Bookmarks bookmarks;
  REQUIRE(session.load_all(tabs, history, bookmarks));
  CHECK(tabs.size() == 2);
  CHECK(tabs.active()->url == "https://two.example");
  CHECK(tabs.active()->title == "Two");
  CHECK(history.size() == 2);
  CHECK(bookmarks.contains("https://two.example"));
}

}  // namespace

TEST_SUITE("session") {
  TEST_CASE("in-memory roundtrip") {
    InMemoryStorage storage;
    {
      TabManager tabs;
      History history;
      Bookmarks bookmarks;
      fill(tabs, history, bookmarks);
      CHECK(Session(storage).save_all(tabs, history, bookmarks));
    }
    CHECK(storage.data().count("tabs") == 1);
    CHECK(storage.data().count("history") == 1);
    CHECK(storage.data().count("bookmarks") == 1);
    check_restored(storage);
  }

  TEST_CASE("missing and malformed data leave objects untouched") {
    InMemoryStorage storage;
    Session session(storage);
    TabManager tabs;
    tabs.open("https://keep");
    CHECK_FALSE(session.load_tabs(tabs));
    storage.write("tabs", "{not json");
    CHECK_FALSE(session.load_tabs(tabs));
    CHECK(tabs.size() == 1);
    CHECK_FALSE(storage.write("../escape", "x"));
  }

  TEST_CASE("file storage roundtrip in a temp dir") {
    char tmpl[] = "/tmp/ps5browser-test-XXXXXX";
    REQUIRE(::mkdtemp(tmpl) != nullptr);
    const std::string dir = std::string(tmpl) + "/nested/state";
    {
      FileStorage storage(dir);
      TabManager tabs;
      History history;
      Bookmarks bookmarks;
      fill(tabs, history, bookmarks);
      CHECK(Session(storage).save_all(tabs, history, bookmarks));
      CHECK(storage.write("tabs", tabs.to_json().dump()));  // overwrite works
      CHECK_FALSE(storage.read("missing").has_value());
      CHECK_FALSE(storage.write("bad/key", "x"));
    }
    FileStorage reopened(dir);
    check_restored(reopened);
    // No temp files left behind.
    CHECK(::access((dir + "/tabs.json.tmp").c_str(), F_OK) != 0);

    for (const char* key : {"tabs", "history", "bookmarks"}) {
      std::remove(reopened.path_for(key).c_str());
    }
    ::rmdir(dir.c_str());
    ::rmdir((std::string(tmpl) + "/nested").c_str());
    ::rmdir(tmpl);
  }
}
