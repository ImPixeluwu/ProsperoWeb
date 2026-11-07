// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cstdint>

#include "prosperoweb/remote/remote_api.h"

using namespace prosperoweb::remote;
using prosperoweb::core::RemoteCommand;
using json = nlohmann::json;

namespace {

Pairing::RandomBytes counter() {
  return [n = std::uint8_t{7}](unsigned char* out, std::size_t size) mutable {
    for (std::size_t i = 0; i < size; ++i) out[i] = n++;
  };
}

HttpRequest req(std::string method, std::string path, std::string body = {},
                std::string token = {}) {
  HttpRequest r;
  r.method = std::move(method);
  r.path = std::move(path);
  r.version = "HTTP/1.1";
  r.body = std::move(body);
  if (!token.empty()) r.headers["x-pw-token"] = token;
  return r;
}

std::string pair(RemoteApi& api) {
  const auto res = api.handle(req("POST", "/api/pair", json{{"pin", api.pin()}}.dump()));
  REQUIRE(res.status == 200);
  return json::parse(res.body)["token"].get<std::string>();
}

}  // namespace

TEST_SUITE("remote.api") {
  TEST_CASE("the phone page needs no token") {
    RemoteApi api(counter());
    const auto res = api.handle(req("GET", "/"));
    CHECK(res.status == 200);
    CHECK(res.content_type.rfind("text/html", 0) == 0);
    CHECK(res.body.find("ProsperoWeb remote") != std::string::npos);
    CHECK(res.body.find("not encrypted") != std::string::npos);
    CHECK(api.handle(req("POST", "/")).status == 405);
    CHECK(api.handle(req("GET", "/nope")).status == 404);
  }

  TEST_CASE("pairing") {
    RemoteApi api(counter());
    CHECK(api.handle(req("POST", "/api/pair", "{\"pin\":\"x\"}")).status == 403);
    CHECK(api.handle(req("POST", "/api/pair", "not json")).status == 403);
    CHECK(api.handle(req("GET", "/api/pair")).status == 405);
    const std::string token = pair(api);
    CHECK(api.devices() == 1);
    CHECK(api.handle(req("GET", "/api/status", {}, token)).status == 200);
    for (int i = 0; i < Pairing::kMaxFailures; ++i) {
      api.handle(req("POST", "/api/pair", "{\"pin\":\"x\"}"));
    }
    const auto locked = api.handle(req("POST", "/api/pair", json{{"pin", api.pin()}}.dump()));
    CHECK(locked.status == 429);
    // Phones already paired keep working.
    CHECK(api.handle(req("GET", "/api/status", {}, token)).status == 200);
    api.start_session();
    CHECK(api.handle(req("GET", "/api/status", {}, token)).status == 401);
    CHECK(api.devices() == 0);
  }

  TEST_CASE("commands are queued for the shell") {
    RemoteApi api(counter());
    const auto body = json{{"type", "click"}}.dump();
    CHECK(api.handle(req("POST", "/api/command", body)).status == 401);
    CHECK(api.handle(req("POST", "/api/command", body, "bad")).status == 401);
    const std::string token = pair(api);
    CHECK(api.handle(req("POST", "/api/command", body, token)).status == 200);
    const auto batch = json::array({{{"type", "pointer"}, {"dx", 3}, {"dy", 4}},
                                    {{"type", "open"}, {"text", "example.com"}}});
    CHECK(api.handle(req("POST", "/api/command", batch.dump(), token)).status == 200);
    auto taken = api.take_commands();
    REQUIRE(taken.size() == 3);
    CHECK(taken[0].type == RemoteCommand::Type::Click);
    CHECK(taken[1].type == RemoteCommand::Type::Pointer);
    CHECK(taken[2].text == "example.com");
    CHECK(api.take_commands().empty());
    // One bad command rejects the whole batch.
    const auto bad = json::array({{{"type", "click"}}, {{"type", "rm -rf"}}});
    CHECK(api.handle(req("POST", "/api/command", bad.dump(), token)).status == 400);
    CHECK(api.take_commands().empty());
    CHECK(api.handle(req("GET", "/api/command", {}, token)).status == 405);
  }

  TEST_CASE("a full queue answers busy") {
    RemoteApi api(counter());
    const std::string token = pair(api);
    const auto click = json{{"type", "click"}}.dump();
    for (std::size_t i = 0; i < RemoteApi::kMaxQueuedCommands; ++i) {
      REQUIRE(api.handle(req("POST", "/api/command", click, token)).status == 200);
    }
    CHECK(api.handle(req("POST", "/api/command", click, token)).status == 503);
    api.take_commands();
    CHECK(api.handle(req("POST", "/api/command", click, token)).status == 200);
  }

  TEST_CASE("status is what the shell set; revoking unpairs") {
    RemoteApi api(counter());
    const std::string token = pair(api);
    api.set_status({{"url", "https://example.com/"}, {"tabs", 2}});
    const auto res = api.handle(req("GET", "/api/status", {}, token));
    CHECK(json::parse(res.body)["url"] == "https://example.com/");
    api.revoke_all();
    CHECK(api.handle(req("GET", "/api/status", {}, token)).status == 401);
  }
}
