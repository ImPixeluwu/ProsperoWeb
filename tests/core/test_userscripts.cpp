// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "prosperoweb/core/in_memory_storage.h"
#include "prosperoweb/core/session.h"
#include "prosperoweb/core/userscripts.h"

using namespace prosperoweb::core;

namespace {

std::string script(const std::string& meta_lines, const std::string& body = "console.log(1);") {
  return "// ==UserScript==\n" + meta_lines + "// ==/UserScript==\n" + body + "\n";
}

const std::string kDark = script(
    "// @name        Dark mode\n"
    "// @namespace   example.org\n"
    "// @version     1.2\n"
    "// @description Makes pages dark\n"
    "// @match       *://*.example.com/*\n"
    "// @include     https://news.test/*\n"
    "// @exclude     *://*.example.com/admin*\n"
    "// @grant       GM_addStyle\n"
    "// @run-at      document-start\n"
    "// @noframes\n");

}  // namespace

TEST_SUITE("userscripts") {
  TEST_CASE("metadata block") {
    const auto m = parse_userscript_meta(kDark);
    REQUIRE(m.has_value());
    CHECK(m->name == "Dark mode");
    CHECK(m->namespace_ == "example.org");
    CHECK(m->version == "1.2");
    CHECK(m->description == "Makes pages dark");
    CHECK(m->matches == std::vector<std::string>{"*://*.example.com/*"});
    CHECK(m->includes == std::vector<std::string>{"https://news.test/*"});
    CHECK(m->excludes == std::vector<std::string>{"*://*.example.com/admin*"});
    CHECK(m->grants == std::vector<std::string>{"GM_addStyle"});
    CHECK(m->run_at == RunAt::DocumentStart);
    CHECK(m->noframes);
    CHECK(m->id() == "example.org/Dark mode");
  }

  TEST_CASE("no block or no name is not a user script") {
    CHECK_FALSE(parse_userscript_meta("console.log('plain js');").has_value());
    CHECK_FALSE(parse_userscript_meta(script("// @version 1\n")).has_value());
    CHECK_FALSE(parse_userscript_meta("// ==UserScript==\n// @name X\n").has_value());
    const auto crlf = parse_userscript_meta("// ==UserScript==\r\n// @name   CRLF\r\n// ==/UserScript==\r\n");
    REQUIRE(crlf.has_value());
    CHECK(crlf->name == "CRLF");
    CHECK(crlf->run_at == RunAt::DocumentEnd);  // the default
  }

  TEST_CASE("match patterns") {
    UserscriptMeta m;
    m.name = "t";
    m.matches = {"*://*.example.com/*"};
    CHECK(userscript_applies(m, "https://example.com/"));
    CHECK(userscript_applies(m, "http://www.example.com/a/b?c=1"));
    CHECK(userscript_applies(m, "https://example.com:8443/x#frag"));
    CHECK_FALSE(userscript_applies(m, "https://notexample.com/"));
    CHECK_FALSE(userscript_applies(m, "https://example.com.evil.test/"));
    CHECK_FALSE(userscript_applies(m, "ftp://example.com/"));
    m.matches = {"https://example.com/watch*"};
    CHECK(userscript_applies(m, "https://example.com/watch?v=1"));
    CHECK_FALSE(userscript_applies(m, "https://example.com/feed"));
    CHECK_FALSE(userscript_applies(m, "http://example.com/watch"));
    CHECK(match_pattern_to_regex("<all_urls>") == "^(?:https?|file|ftp)://");
    CHECK(match_pattern_to_regex("example.com/*").empty());
    CHECK(match_pattern_to_regex("https://exa*mple.com/*").empty());
    CHECK(match_pattern_to_regex("https://example.com").empty());
  }

  TEST_CASE("include globs, regexes and excludes") {
    UserscriptMeta m;
    m.name = "t";
    m.includes = {"http*://forum.test/thread/*"};
    CHECK(userscript_applies(m, "https://forum.test/thread/42"));
    CHECK_FALSE(userscript_applies(m, "https://forum.test/"));
    m.includes = {"/^https://(a|b)\\.test//"};
    CHECK(userscript_applies(m, "https://b.test/x"));
    CHECK_FALSE(userscript_applies(m, "https://c.test/x"));
    m.includes = {"/[broken/"};  // an invalid regex never matches
    CHECK_FALSE(userscript_applies(m, "https://b.test/x"));
    const auto dark = parse_userscript_meta(kDark);
    REQUIRE(dark.has_value());
    CHECK(userscript_applies(*dark, "https://news.test/today"));
    CHECK(userscript_applies(*dark, "https://www.example.com/blog"));
    CHECK_FALSE(userscript_applies(*dark, "https://www.example.com/admin/users"));
  }

  TEST_CASE("without @match or @include a script runs on every web page") {
    UserscriptMeta m;
    m.name = "everywhere";
    CHECK(userscript_applies(m, "https://any.test/"));
    CHECK(userscript_applies(m, "http://any.test/"));
    CHECK_FALSE(userscript_applies(m, "about:blank"));
    CHECK_FALSE(userscript_applies(m, "file:///data/x.html"));
  }

  TEST_CASE("the wrapper carries the rules, the GM API and the source") {
    const auto m = parse_userscript_meta(kDark);
    REQUIRE(m.has_value());
    const std::string js = userscript_wrapper(*m, "// trailing comment");
    CHECK(js.find("\"example.org/Dark mode\"") != std::string::npos);
    // The regex is embedded as a JSON string (backslashes escaped).
    CHECK(js.find(nlohmann::json(match_pattern_to_regex("*://*.example.com/*")).dump()) !=
          std::string::npos);
    CHECK(js.find("function GM_addStyle") != std::string::npos);
    CHECK(js.find("var NOFRAMES = true") != std::string::npos);
    CHECK(js.find("var RUN_AT = 'start'") != std::string::npos);
    // The trailing line comment ends before the closing code.
    CHECK(js.find("// trailing comment\n") != std::string::npos);
    CHECK(js.ends_with("})();\n"));
  }

  TEST_CASE("store: install, update by id, toggle, remove") {
    UserscriptStore s;
    CHECK_FALSE(s.install("console.log('not a userscript');").has_value());
    const auto i = s.install(kDark);
    REQUIRE(i == 0u);
    REQUIRE(s.set_enabled(0, false));
    const std::string v2 = kDark;
    const auto updated = s.install(std::string(v2).replace(v2.find("1.2"), 3, "2.0"));
    CHECK(updated == 0u);
    CHECK(s.size() == 1);
    CHECK(s.scripts()[0].meta.version == "2.0");
    CHECK_FALSE(s.scripts()[0].enabled);  // an update keeps the state
    CHECK(s.injected_sources().empty());
    s.set_enabled(0, true);
    CHECK(s.injected_sources().size() == 1);
    CHECK(s.count_for_url("https://news.test/") == 1);
    CHECK(s.count_for_url("https://other.test/") == 0);
    CHECK(s.remove(0));
    CHECK_FALSE(s.remove(0));
    CHECK(s.size() == 0);
    CHECK_FALSE(s.install(std::string(kMaxUserscriptBytes + 1, ' ') + kDark).has_value());
  }

  TEST_CASE("store json and Session") {
    UserscriptStore s;
    s.install(kDark);
    s.set_enabled(0, false);
    UserscriptStore back;
    REQUIRE(back.from_json(s.to_json()));
    REQUIRE(back.size() == 1);
    CHECK_FALSE(back.scripts()[0].enabled);
    CHECK_FALSE(back.from_json(nlohmann::json::object()));
    REQUIRE(back.from_json(nlohmann::json::parse(R"([{"source":"no meta"},{"enabled":true},7])")));
    CHECK(back.size() == 0);

    InMemoryStorage storage;
    Session session(storage);
    REQUIRE(session.save_userscripts(s));
    UserscriptStore loaded;
    REQUIRE(session.load_userscripts(loaded));
    CHECK(loaded.size() == 1);
  }

  TEST_CASE("userscript URLs") {
    CHECK(is_userscript_url("https://greasyfork.org/scripts/1-x/code/X.user.js"));
    CHECK(is_userscript_url("http://host.test/a.USER.JS?v=2#x"));
    CHECK_FALSE(is_userscript_url("https://host.test/a.js"));
    CHECK_FALSE(is_userscript_url("https://host.test/?file=a.user.js"));
    CHECK_FALSE(is_userscript_url("file:///data/a.user.js"));
    CHECK(userscript_source_script().find("userscript_source") != std::string::npos);
  }
}
