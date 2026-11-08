// SPDX-License-Identifier: GPL-3.0-or-later
//
// PIN pairing for the remote control. Every start draws a new six-digit
// PIN; a phone that sends it gets a random token, which every later request
// must carry. After kMaxFailures wrong PINs every PIN is refused until the
// next reset(), so the PIN cannot be guessed over the network.
#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace prosperoweb::remote {

class Pairing {
 public:
  using RandomBytes = std::function<void(unsigned char* out, std::size_t size)>;

  static constexpr int kMaxFailures = 5;
  static constexpr std::size_t kMaxDevices = 8;
  static constexpr std::size_t kTokenBytes = 16;  // 32 hex characters

  explicit Pairing(RandomBytes random);

  // A new PIN; forgets every token and failure. Returns the PIN.
  const std::string& reset();
  const std::string& pin() const { return pin_; }

  // A token for the right PIN; nullopt otherwise (a failure is counted).
  // When kMaxDevices phones are paired, the oldest token is dropped.
  std::optional<std::string> pair(std::string_view pin);
  bool valid(std::string_view token) const;
  bool locked() const { return failures_ >= kMaxFailures; }

  std::size_t devices() const { return tokens_.size(); }
  void revoke_all() { tokens_.clear(); }

 private:
  std::string random_hex(std::size_t bytes);

  RandomBytes random_;
  std::string pin_;
  std::vector<std::string> tokens_;
  int failures_ = 0;
};

// Constant-time comparison (no early exit on the first difference).
bool same_secret(std::string_view a, std::string_view b);

}  // namespace prosperoweb::remote
