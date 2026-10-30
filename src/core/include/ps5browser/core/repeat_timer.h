// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace ps5browser::core {

// Auto-repeat for held buttons: fires once on press, then after
// initial_delay_ms, then every interval_ms while held.
class RepeatTimer {
 public:
  static constexpr std::uint32_t kDefaultInitialDelayMs = 300;
  static constexpr std::uint32_t kDefaultIntervalMs = 60;

  explicit RepeatTimer(std::uint32_t initial_delay_ms = kDefaultInitialDelayMs,
                       std::uint32_t interval_ms = kDefaultIntervalMs);

  // Starts holding. Returns true if this is a new press (fire once now).
  bool press();
  void release();
  bool held() const { return held_; }

  // Advances time; returns how many repeats fired during dt_ms.
  std::uint32_t update(std::uint32_t dt_ms);

 private:
  std::uint32_t initial_delay_ms_;
  std::uint32_t interval_ms_;
  bool held_ = false;
  std::uint64_t elapsed_ms_ = 0;
  std::uint64_t next_fire_ms_ = 0;
};

}  // namespace ps5browser::core
