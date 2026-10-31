// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

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
    CHECK(renamed_env_name("PS5BROWSER_CA_BUNDLE") == "PROSPEROWEB_CA_BUNDLE");
    CHECK(renamed_env_name("PS5BROWSER_") == "");
    CHECK(renamed_env_name("PROSPEROWEB_CA_BUNDLE") == "");
    CHECK(renamed_env_name("HOME") == "");
  }

  TEST_CASE("legacy env vars are adopted unless the new name is set") {
    ::setenv("PS5BROWSER_TEST_ADOPT", "old", 1);
    ::unsetenv("PROSPEROWEB_TEST_ADOPT");
    ::setenv("PS5BROWSER_TEST_KEEP", "old", 1);
    ::setenv("PROSPEROWEB_TEST_KEEP", "new", 1);
    const auto adopted = adopt_legacy_env();
    CHECK(std::string(std::getenv("PROSPEROWEB_TEST_ADOPT")) == "old");
    CHECK(std::string(std::getenv("PROSPEROWEB_TEST_KEEP")) == "new");
    bool saw_adopt = false;
    bool saw_keep = false;
    for (const auto& name : adopted) {
      saw_adopt = saw_adopt || name == "PS5BROWSER_TEST_ADOPT";
      saw_keep = saw_keep || name == "PS5BROWSER_TEST_KEEP";
    }
    CHECK(saw_adopt);
    CHECK_FALSE(saw_keep);
    CHECK(adopt_legacy_env().empty());  // nothing left to adopt
    for (const char* name : {"PS5BROWSER_TEST_ADOPT", "PROSPEROWEB_TEST_ADOPT",
                             "PS5BROWSER_TEST_KEEP", "PROSPEROWEB_TEST_KEEP"}) {
      ::unsetenv(name);
    }
  }

  TEST_CASE("the profile is copied once, missing items only") {
    const std::string root = make_temp_dir();
    const std::string from = root + "/ps5browser";
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

    const auto first = migrate_legacy_profile(from, to);
    CHECK(first.ran);
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
    const auto second = migrate_legacy_profile(from, to);
    CHECK_FALSE(second.ran);
    CHECK(second.copied == 0);
    CHECK_FALSE(exists(to + "/tabs.json"));
  }

  TEST_CASE("a missing new profile directory is created, config.json paths follow") {
    const std::string root = make_temp_dir();
    const std::string from = root + "/old";
    REQUIRE(::mkdir(from.c_str(), 0755) == 0);
    write_file(from + "/config.json",
               "{\"chrome_dir\": \"" + from + "/chrome\", \"home\": \"" + from +
                   "\", \"other\": \"" + from + "x/y\", \"url\": \"file://" + from + "/a\"}");
    const std::string to = root + "/a/b";
    const auto result = migrate_legacy_profile(from, to);
    CHECK(result.ran);
    CHECK(result.failed.empty());
    CHECK(result.copied == 1);
    CHECK(read_file(to + "/config.json") ==
          "{\"chrome_dir\": \"" + to + "/chrome\", \"home\": \"" + to + "\", \"other\": \"" +
              from + "x/y\", \"url\": \"file://" + to + "/a\"}");
  }

  TEST_CASE("no legacy profile, nothing happens") {
    const std::string root = make_temp_dir();
    const auto result = migrate_legacy_profile(root + "/missing", root + "/to");
    CHECK_FALSE(result.ran);
    CHECK_FALSE(exists(root + "/to"));
  }
}
