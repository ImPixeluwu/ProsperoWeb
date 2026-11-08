// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "prosperoweb/core/remote_command.h"

using namespace prosperoweb::core;
using json = nlohmann::json;

TEST_SUITE("remote_command") {
  TEST_CASE("open and text") {
    auto c = parse_remote_command(json{{"type", "open"}, {"text", "example.com"}, {"new_tab", true}});
    REQUIRE(c.has_value());
    CHECK(c->type == RemoteCommand::Type::OpenUrl);
    CHECK(c->text == "example.com");
    CHECK(c->new_tab);
    c = parse_remote_command(json{{"type", "text"}, {"text", "hola"}, {"submit", true}});
    REQUIRE(c.has_value());
    CHECK(c->type == RemoteCommand::Type::Text);
    CHECK(c->submit);
    CHECK_FALSE(c->new_tab);
  }

  TEST_CASE("text is required, non-empty and bounded") {
    CHECK_FALSE(parse_remote_command(json{{"type", "open"}}).has_value());
    CHECK_FALSE(parse_remote_command(json{{"type", "text"}, {"text", ""}}).has_value());
    CHECK_FALSE(
        parse_remote_command(json{{"type", "open"}, {"text", ""}, {"submit", true}}).has_value());
    auto enter = parse_remote_command(json{{"type", "text"}, {"text", ""}, {"submit", true}});
    REQUIRE(enter.has_value());
    CHECK(enter->text.empty());
    CHECK_FALSE(parse_remote_command(json{{"type", "text"}, {"text", 5}}).has_value());
    CHECK_FALSE(parse_remote_command(
                    json{{"type", "text"}, {"text", std::string(kMaxRemoteTextBytes + 1, 'a')}})
                    .has_value());
  }

  TEST_CASE("pointer and scroll deltas, clamped") {
    auto c = parse_remote_command(json{{"type", "pointer"}, {"dx", 12.5}, {"dy", -3}});
    REQUIRE(c.has_value());
    CHECK(c->type == RemoteCommand::Type::Pointer);
    CHECK(c->dx == doctest::Approx(12.5f));
    CHECK(c->dy == doctest::Approx(-3.0f));
    c = parse_remote_command(json{{"type", "scroll"}, {"dy", 1e9}});
    REQUIRE(c.has_value());
    CHECK(c->dx == 0.0f);
    CHECK(c->dy == doctest::Approx(kMaxRemoteDelta));
    CHECK_FALSE(parse_remote_command(json{{"type", "pointer"}, {"dx", "far"}}).has_value());
  }

  TEST_CASE("click and actions") {
    CHECK(parse_remote_command(json{{"type", "click"}})->type == RemoteCommand::Type::Click);
    auto c = parse_remote_command(json{{"type", "action"}, {"name", "next_tab"}});
    REQUIRE(c.has_value());
    CHECK(c->action == RemoteAction::NextTab);
    for (const char* n : {"back", "forward", "reload", "prev_tab", "next_tab", "new_tab",
                          "close_tab", "page_up", "page_down", "fullscreen", "menu", "escape"}) {
      CAPTURE(n);
      CHECK(remote_action_from_name(n).has_value());
    }
    CHECK_FALSE(parse_remote_command(json{{"type", "action"}, {"name", "self_destruct"}}).has_value());
  }

  TEST_CASE("anything else is rejected") {
    CHECK_FALSE(parse_remote_command(json::array()).has_value());
    CHECK_FALSE(parse_remote_command(json{{"type", "format_disk"}}).has_value());
    CHECK_FALSE(parse_remote_command(json{{"text", "x"}}).has_value());
  }
}
