// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5browser/core/repeat_timer.h"

#include <algorithm>

namespace ps5browser::core {

RepeatTimer::RepeatTimer(std::uint32_t initial_delay_ms, std::uint32_t interval_ms)
    : initial_delay_ms_(initial_delay_ms), interval_ms_(std::max<std::uint32_t>(1, interval_ms)) {}

bool RepeatTimer::press() {
  if (held_) {
    return false;
  }
  held_ = true;
  elapsed_ms_ = 0;
  next_fire_ms_ = initial_delay_ms_;
  return true;
}

void RepeatTimer::release() { held_ = false; }

std::uint32_t RepeatTimer::update(std::uint32_t dt_ms) {
  if (!held_) {
    return 0;
  }
  elapsed_ms_ += dt_ms;
  std::uint32_t fired = 0;
  while (elapsed_ms_ >= next_fire_ms_) {
    ++fired;
    next_fire_ms_ += interval_ms_;
  }
  return fired;
}

}  // namespace ps5browser::core
