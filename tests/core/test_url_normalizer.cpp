// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "prosperoweb/core/url_normalizer.h"

using prosperoweb::core::UrlNormalizer;

TEST_SUITE("url_normalizer") {
  TEST_CASE("pass-through schemes") {
    CHECK(UrlNormalizer::normalize("about:blank") == "about:blank");
    CHECK(UrlNormalizer::normalize("  https://example.com/a?b=c  ") == "https://example.com/a?b=c");
    CHECK(UrlNormalizer::normalize("http://example.com") == "http://example.com");
    CHECK(UrlNormalizer::normalize("HTTPS://Example.com") == "HTTPS://Example.com");
    CHECK(UrlNormalizer::normalize("file:///data/index.html") == "file:///data/index.html");
    CHECK(UrlNormalizer::normalize("ftp://ftp.example.com") == "ftp://ftp.example.com");
  }

  TEST_CASE("bare hosts get https") {
    CHECK(UrlNormalizer::normalize("example.com") == "https://example.com");
    CHECK(UrlNormalizer::normalize("www.youtube.com/watch?v=x") == "https://www.youtube.com/watch?v=x");
    CHECK(UrlNormalizer::normalize("example.com:8443/path") == "https://example.com:8443/path");
  }

  TEST_CASE("localhost and IPv4 get http") {
    CHECK(UrlNormalizer::normalize("localhost") == "http://localhost");
    CHECK(UrlNormalizer::normalize("localhost:8080/index.html") == "http://localhost:8080/index.html");
    CHECK(UrlNormalizer::normalize("192.168.1.10") == "http://192.168.1.10");
    CHECK(UrlNormalizer::normalize("10.0.0.1:9020") == "http://10.0.0.1:9020");
    // Not a valid IPv4 literal: treated like any dotted host.
    CHECK(UrlNormalizer::normalize("999.1.1.1") == "https://999.1.1.1");
  }

  TEST_CASE("search fallback") {
    CHECK(UrlNormalizer::normalize("") == "about:blank");
    CHECK(UrlNormalizer::normalize("weather") == "https://duckduckgo.com/?q=weather");
    CHECK(UrlNormalizer::normalize("ps5 homebrew") == "https://duckduckgo.com/?q=ps5%20homebrew");
    CHECK(UrlNormalizer::normalize("what is example.com") ==
          "https://duckduckgo.com/?q=what%20is%20example.com");
    CHECK(UrlNormalizer::normalize("a&b=c") == "https://duckduckgo.com/?q=a%26b%3Dc");
    CHECK(UrlNormalizer::normalize("привет") ==
          "https://duckduckgo.com/?q=%D0%BF%D1%80%D0%B8%D0%B2%D0%B5%D1%82");
    CHECK(UrlNormalizer::normalize("x y", "https://search.example/s?q={}&lang=en") ==
          "https://search.example/s?q=x%20y&lang=en");
    CHECK(UrlNormalizer::normalize("x", "https://search.example/s?q=") ==
          "https://search.example/s?q=x");
  }

  TEST_CASE("display_host") {
    CHECK(UrlNormalizer::display_host("https://User:pw@WWW.Example.com:8080/a?b#c") ==
          "www.example.com");
    CHECK(UrlNormalizer::display_host("http://[::1]:8080/") == "[::1]");
    CHECK(UrlNormalizer::display_host("about:blank") == "about:blank");
    CHECK(UrlNormalizer::display_host("file:///data/x.html") == "file:///data/x.html");
  }
}
