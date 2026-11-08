// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/remote/pairing.h"

#include <cstdint>

namespace prosperoweb::remote {

Pairing::Pairing(RandomBytes random) : random_(std::move(random)) { reset(); }

const std::string& Pairing::reset() {
  // Uniform 000000..999999 by rejection sampling on 32-bit draws.
  constexpr std::uint32_t kRange = 1000000;
  constexpr std::uint32_t kLimit = UINT32_MAX - (UINT32_MAX % kRange);
  std::uint32_t v = 0;
  do {
    unsigned char b[4];
    random_(b, sizeof b);
    v = static_cast<std::uint32_t>(b[0]) | static_cast<std::uint32_t>(b[1]) << 8 |
        static_cast<std::uint32_t>(b[2]) << 16 | static_cast<std::uint32_t>(b[3]) << 24;
  } while (v >= kLimit);
  std::string pin = std::to_string(v % kRange);
  pin_ = std::string(6 - pin.size(), '0') + pin;
  tokens_.clear();
  failures_ = 0;
  return pin_;
}

std::string Pairing::random_hex(std::size_t bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::vector<unsigned char> raw(bytes);
  random_(raw.data(), raw.size());
  std::string out;
  out.reserve(bytes * 2);
  for (unsigned char c : raw) {
    out += kHex[c >> 4];
    out += kHex[c & 15];
  }
  return out;
}

std::optional<std::string> Pairing::pair(std::string_view pin) {
  if (locked()) {
    return std::nullopt;
  }
  if (!same_secret(pin, pin_)) {
    ++failures_;
    return std::nullopt;
  }
  if (tokens_.size() >= kMaxDevices) {
    tokens_.erase(tokens_.begin());
  }
  tokens_.push_back(random_hex(kTokenBytes));
  return tokens_.back();
}

bool Pairing::valid(std::string_view token) const {
  bool ok = false;
  for (const auto& t : tokens_) {
    ok = same_secret(token, t) || ok;  // no short-circuit: same time for every token
  }
  return ok;
}

bool same_secret(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  }
  return diff == 0;
}

}  // namespace prosperoweb::remote
