// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "prosperoweb/core/legacy_profile.h"

using namespace prosperoweb::core;

namespace {

std::string make_temp_dir() {
  char tmpl[] = "/tmp/prosperoweb-legacy-XXXXXX";
  REQUIRE(::mkdtemp(tmpl) != nullptr);
  return tmpl;
}

void write_file(const std::string& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

bool exists(const std::string& path) { return ::access(path.c_str(), F_OK) == 0; }

}  // namespace

TEST_SUITE("legacy_profile") {
  TEST_CASE("legacy env names map to the new prefix") {
    CHECK(renamed_env_name("PROSPEROBROWSER_CA_BUNDLE") == "PROSPEROWEB_CA_BUNDLE");
    CHECK(renamed_env_name("PS5BROWSER_CA_BUNDLE") == "PROSPEROWEB_CA_BUNDLE");
    CHECK(renamed_env_name("PROSPEROBROWSER_") == "");
    CHECK(renamed_env_name("PS5BROWSER_") == "");
    CHECK(renamed_env_name("PROSPEROWEB_CA_BUNDLE") == "");
    CHECK(renamed_env_name("HOME") == "");
  }

  TEST_CASE("newer names win over older legacy names") {
    CHECK(env_names_preferred_over("PS5BROWSER_X") ==
          std::vector<std::string>{"PROSPEROWEB_X", "PROSPEROBROWSER_X"});
    CHECK(env_names_preferred_over("PROSPEROBROWSER_X") ==
          std::vector<std::string>{"PROSPEROWEB_X"});
    CHECK(env_names_preferred_over("PROSPEROWEB_X").empty());
    CHECK(env_names_preferred_over("HOME").empty());
  }

  TEST_CASE("legacy env vars are adopted unless a newer name is set") {
    ::setenv("PS5BROWSER_TEST_ADOPT", "old", 1);
    ::unsetenv("PROSPEROWEB_TEST_ADOPT");
    ::setenv("PROSPEROBROWSER_TEST_RECENT", "recent", 1);
    ::unsetenv("PROSPEROWEB_TEST_RECENT");
    ::setenv("PS5BROWSER_TEST_BOTH", "oldest", 1);
    ::setenv("PROSPEROBROWSER_TEST_BOTH", "recent", 1);
    ::unsetenv("PROSPEROWEB_TEST_BOTH");
    ::setenv("PS5BROWSER_TEST_KEEP", "old", 1);
    ::setenv("PROSPEROBROWSER_TEST_KEEP", "recent", 1);
    ::setenv("PROSPEROWEB_TEST_KEEP", "new", 1);
    const auto adopted = adopt_legacy_env();
    CHECK(std::string(std::getenv("PROSPEROWEB_TEST_ADOPT")) == "old");
    CHECK(std::string(std::getenv("PROSPEROWEB_TEST_RECENT")) == "recent");
    CHECK(std::string(std::getenv("PROSPEROWEB_TEST_BOTH")) == "recent");
    CHECK(std::string(std::getenv("PROSPEROWEB_TEST_KEEP")) == "new");
    std::vector<std::string> ours;
    for (const auto& name : adopted) {
      if (name.find("_TEST_") != std::string::npos) {
        ours.push_back(name);
      }
    }
    CHECK(ours == std::vector<std::string>{"PROSPEROBROWSER_TEST_BOTH",
                                           "PROSPEROBROWSER_TEST_RECENT",
                                           "PS5BROWSER_TEST_ADOPT"});
    CHECK(adopt_legacy_env().empty());  // nothing left to adopt
    for (const char* suffix : {"ADOPT", "RECENT", "BOTH", "KEEP"}) {
      for (const char* prefix : {"PS5BROWSER_TEST_", "PROSPEROBROWSER_TEST_", "PROSPEROWEB_TEST_"}) {
        ::unsetenv((std::string(prefix) + suffix).c_str());
      }
    }
  }

  TEST_CASE("the profile is copied once, missing items only") {
    const std::string root = make_temp_dir();
    const std::string from = root + "/prosperobrowser";
    const std::string to = root + "/new/prosperoweb";
    REQUIRE(::mkdir(from.c_str(), 0755) == 0);
    write_file(from + "/tabs.json", "old tabs");
    write_file(from + "/history.json", "old history");
    write_file(from + "/log.txt", "not carried over");
    REQUIRE(::mkdir((from + "/storage").c_str(), 0755) == 0);
    REQUIRE(::mkdir((from + "/storage/nested").c_str(), 0755) == 0);
    write_file(from + "/storage/salt", "salt");
    write_file(from + "/storage/nested/db", "db");

    // The new profile already has its own history.
    REQUIRE(::mkdir((root + "/new").c_str(), 0755) == 0);
    REQUIRE(::mkdir(to.c_str(), 0755) == 0);
    write_file(to + "/history.json", "new history");

    const auto first = migrate_legacy_profile({from}, to);
    CHECK(first.ran);
    CHECK(first.from_dir == from);
    CHECK(first.failed.empty());
    CHECK(first.copied == 2);  // tabs.json, storage
    CHECK(read_file(to + "/tabs.json") == "old tabs");
    CHECK(read_file(to + "/history.json") == "new history");
    CHECK(read_file(to + "/storage/salt") == "salt");
    CHECK(read_file(to + "/storage/nested/db") == "db");
    CHECK_FALSE(exists(to + "/log.txt"));
    CHECK_FALSE(exists(to + "/tabs.json.tmp"));
    CHECK(exists(to + "/" + kLegacyMigrationMarker));
    // Copied, not moved.
    CHECK(read_file(from + "/tabs.json") == "old tabs");

    // Second start: the marker stops it, even if an item went missing.
    REQUIRE(::unlink((to + "/tabs.json").c_str()) == 0);
    const auto second = migrate_legacy_profile({from}, to);
    CHECK_FALSE(second.ran);
    CHECK(second.copied == 0);
    CHECK_FALSE(exists(to + "/tabs.json"));
  }

  TEST_CASE("the newest old profile is the source, older ones are not mixed in") {
    const std::string root = make_temp_dir();
    const std::string recent = root + "/prosperobrowser";
    const std::string oldest = root + "/ps5browser";
    const std::string to = root + "/prosperoweb";
    REQUIRE(::mkdir(recent.c_str(), 0755) == 0);
    REQUIRE(::mkdir(oldest.c_str(), 0755) == 0);
    write_file(recent + "/tabs.json", "recent tabs");
    write_file(oldest + "/tabs.json", "oldest tabs");
    write_file(oldest + "/bookmarks.json", "oldest bookmarks");
    const auto result = migrate_legacy_profile({recent, oldest}, to);
    CHECK(result.ran);
    CHECK(result.from_dir == recent);
    CHECK(result.copied == 1);
    CHECK(read_file(to + "/tabs.json") == "recent tabs");
    CHECK_FALSE(exists(to + "/bookmarks.json"));
  }

  TEST_CASE("an old directory without a profile falls through to an older one") {
    const std::string root = make_temp_dir();
    const std::string recent = root + "/prosperobrowser";
    const std::string oldest = root + "/ps5browser";
    const std::string to = root + "/prosperoweb";
    REQUIRE(::mkdir(recent.c_str(), 0755) == 0);  // installed, never started
    write_file(recent + "/prosperobrowser.elf", "elf");
    REQUIRE(::mkdir(oldest.c_str(), 0755) == 0);
    write_file(oldest + "/bookmarks.json", "oldest bookmarks");
    const auto result = migrate_legacy_profile({recent, root + "/missing", oldest}, to);
    CHECK(result.ran);
    CHECK(result.from_dir == oldest);
    CHECK(result.copied == 1);
    CHECK(read_file(to + "/bookmarks.json") == "oldest bookmarks");
    CHECK_FALSE(exists(to + "/prosperobrowser.elf"));
  }

  TEST_CASE("a missing new profile directory is created, config.json paths follow") {
    const std::string root = make_temp_dir();
    const std::string from = root + "/old";
    const std::string older = root + "/older";
    REQUIRE(::mkdir(from.c_str(), 0755) == 0);
    write_file(from + "/config.json",
               "{\"chrome_dir\": \"" + from + "/chrome\", \"home\": \"" + from +
                   "\", \"other\": \"" + from + "x/y\", \"url\": \"file://" + from +
                   "/a\", \"kept\": \"" + older + "/b\"}");
    const std::string to = root + "/a/b";
    const auto result = migrate_legacy_profile({from, older}, to);
    CHECK(result.ran);
    CHECK(result.failed.empty());
    CHECK(result.copied == 1);
    // Both old roots are rewritten, the older one too (left over from an
    // earlier carry-over).
    CHECK(read_file(to + "/config.json") ==
          "{\"chrome_dir\": \"" + to + "/chrome\", \"home\": \"" + to + "\", \"other\": \"" +
              from + "x/y\", \"url\": \"file://" + to + "/a\", \"kept\": \"" + to + "/b\"}");
  }

  TEST_CASE("no legacy profile, nothing happens") {
    const std::string root = make_temp_dir();
    const auto result = migrate_legacy_profile({root + "/missing", root + "/also-missing"},
                                               root + "/to");
    CHECK_FALSE(result.ran);
    CHECK(result.from_dir.empty());
    CHECK_FALSE(exists(root + "/to"));
  }
}
