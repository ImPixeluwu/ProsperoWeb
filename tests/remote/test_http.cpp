// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "prosperoweb/remote/http.h"

using namespace prosperoweb::remote;

TEST_SUITE("remote.http") {
  TEST_CASE("a GET with headers and a query") {
    const std::string raw =
        "GET /api/status?x=1 HTTP/1.1\r\nHost: tv\r\nX-PW-Token: abc\r\n\r\nGET / HTTP/1.1\r\n";
    HttpRequest r;
    std::size_t used = 0;
    REQUIRE(parse_http_request(raw, 1024, r, used) == ParseStatus::Complete);
    CHECK(r.method == "GET");
    CHECK(r.path == "/api/status");
    CHECK(r.query == "x=1");
    CHECK(r.header("x-pw-token") == "abc");
    CHECK(r.header("missing").empty());
    CHECK(r.keep_alive());
    CHECK(raw.substr(used) == "GET / HTTP/1.1\r\n");
  }

  TEST_CASE("a POST body waits for Content-Length bytes") {
    std::string raw = "POST /api/pair HTTP/1.1\r\nContent-Length: 15\r\nConnection: close\r\n\r\n{\"pin\":";
    HttpRequest r;
    std::size_t used = 0;
    CHECK(parse_http_request(raw, 1024, r, used) == ParseStatus::Incomplete);
    raw += "\"12345\"}";
    REQUIRE(parse_http_request(raw, 1024, r, used) == ParseStatus::Complete);
    CHECK(r.body == "{\"pin\":\"12345\"}");
    CHECK_FALSE(r.keep_alive());
    CHECK(used == raw.size());
  }

  TEST_CASE("limits and malformed requests") {
    HttpRequest r;
    std::size_t used = 0;
    CHECK(parse_http_request("POST / HTTP/1.1\r\nContent-Length: 99999\r\n\r\n", 1024, r, used) ==
          ParseStatus::TooLarge);
    CHECK(parse_http_request("POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", 1024, r,
                             used) == ParseStatus::Bad);
    CHECK(parse_http_request("garbage\r\n\r\n", 1024, r, used) == ParseStatus::Bad);
    CHECK(parse_http_request("POST / HTTP/1.1\r\nContent-Length: x\r\n\r\n", 1024, r, used) ==
          ParseStatus::Bad);
    CHECK(parse_http_request(std::string(kMaxHeaderBytes + 10, 'a'), 1024, r, used) ==
          ParseStatus::TooLarge);
    CHECK(parse_http_request("GET / HTTP/1.0\r\n\r\n", 1024, r, used) == ParseStatus::Complete);
    CHECK_FALSE(r.keep_alive());  // HTTP/1.0 closes by default
  }

  TEST_CASE("responses") {
    const std::string s = HttpResponse::json(404, {{"error", "x"}}).serialize(true);
    CHECK(s.rfind("HTTP/1.1 404 Not Found\r\n", 0) == 0);
    CHECK(s.find("Content-Type: application/json") != std::string::npos);
    CHECK(s.find("Content-Length: 13\r\n") != std::string::npos);
    CHECK(s.find("Connection: keep-alive") != std::string::npos);
    CHECK(s.find("Cache-Control: no-store") != std::string::npos);
    CHECK(s.substr(s.size() - 13) == "{\"error\":\"x\"}");
    CHECK(HttpResponse::text(200, "hi").serialize(false).find("Connection: close") !=
          std::string::npos);
  }
}
