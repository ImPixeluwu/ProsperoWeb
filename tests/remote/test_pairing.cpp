// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cstdint>
#include <set>

#include "prosperoweb/remote/pairing.h"

using namespace prosperoweb::remote;

namespace {

// Deterministic "random" bytes: a counter.
Pairing::RandomBytes counter() {
  return [n = std::uint8_t{0}](unsigned char* out, std::size_t size) mutable {
    for (std::size_t i = 0; i < size; ++i) out[i] = n++;
  };
}

}  // namespace

TEST_SUITE("remote.pairing") {
  TEST_CASE("six-digit PIN, new on reset") {
    Pairing p(counter());
    const std::string first = p.pin();
    CHECK(first.size() == 6);
    CHECK(first.find_first_not_of("0123456789") == std::string::npos);
    CHECK(p.reset() != first);
  }

  TEST_CASE("the right PIN gives a token") {
    Pairing p(counter());
    CHECK_FALSE(p.pair("nope").has_value());
    const auto token = p.pair(p.pin());
    REQUIRE(token.has_value());
    CHECK(token->size() == Pairing::kTokenBytes * 2);
    CHECK(p.valid(*token));
    CHECK_FALSE(p.valid("0123"));
    CHECK_FALSE(p.valid(""));
    CHECK(p.devices() == 1);
    p.revoke_all();
    CHECK_FALSE(p.valid(*token));
    CHECK(p.devices() == 0);
  }

  TEST_CASE("locked after too many wrong PINs, until reset") {
    Pairing p(counter());
    const std::string wrong = p.pin() == "000000" ? "111111" : "000000";
    for (int i = 0; i < Pairing::kMaxFailures; ++i) CHECK_FALSE(p.pair(wrong).has_value());
    CHECK(p.locked());
    CHECK_FALSE(p.pair(p.pin()).has_value());
    p.reset();
    CHECK_FALSE(p.locked());
    CHECK(p.pair(p.pin()).has_value());
  }

  TEST_CASE("the oldest phone is dropped beyond kMaxDevices; reset forgets all") {
    Pairing p(counter());
    const auto first = p.pair(p.pin());
    std::set<std::string> tokens{*first};
    for (std::size_t i = 1; i <= Pairing::kMaxDevices; ++i) tokens.insert(*p.pair(p.pin()));
    CHECK(tokens.size() == Pairing::kMaxDevices + 1);
    CHECK(p.devices() == Pairing::kMaxDevices);
    CHECK_FALSE(p.valid(*first));
    const auto last = p.pair(p.pin());
    p.reset();
    CHECK_FALSE(p.valid(*last));
  }

  TEST_CASE("same_secret") {
    CHECK(same_secret("abc", "abc"));
    CHECK_FALSE(same_secret("abc", "abd"));
    CHECK_FALSE(same_secret("abc", "ab"));
    CHECK(same_secret("", ""));
  }
}
