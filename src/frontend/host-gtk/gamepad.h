// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "prosperoweb/core/gamepad.h"
#include "prosperoweb/core/touchpad_model.h"

namespace prosperoweb::host {

// Physical game controller (SDL2 GameController when built with SDL2,
// otherwise a stub that never reports a device).
class Gamepad {
 public:
  Gamepad();
  ~Gamepad();

  Gamepad(const Gamepad&) = delete;
  Gamepad& operator=(const Gamepad&) = delete;

  // Pumps controller events: hot-plug, button changes (reported through
  // on_button) and stick positions (lx, ly, rx, ry in [-1, 1]).
  void poll(const std::function<void(core::Button, bool)>& on_button);
  bool connected() const;
  const float* sticks() const { return sticks_; }
  // Fingers on the controller's touchpad after the last poll() (SDL reports
  // them normalized; a DualSense over USB or Bluetooth has one touchpad).
  const std::vector<core::TouchContact>& touch() const { return touch_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  float sticks_[4] = {0, 0, 0, 0};
  std::vector<core::TouchContact> touch_;
};

}  // namespace prosperoweb::host
