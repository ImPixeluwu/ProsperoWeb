// SPDX-License-Identifier: GPL-3.0-or-later
//
// DualSense touchpad fingers for core::TouchpadModel.
//
// SDL's PS5 joystick driver (the SDK's libSDL2.a, SDL_ps5joystick.c) reads
// the pad with scePadReadState() but never reports the touchpad: it sets no
// ntouchpads and never calls SDL_PrivateJoystickTouchpad(), so no
// SDL_CONTROLLERTOUCHPAD* events arrive and SDL_GameControllerGetNumTouchpads()
// is 0. The fingers are then read from the pad SDL opened: scePadGetHandle()
// finds its handle and scePadReadState() returns the latest state without
// consuming anything SDL needs. Should SDL ever report a touchpad, it is used
// instead.
#pragma once

#include <cstdint>
#include <vector>

#include <SDL.h>

#include "prosperobrowser/core/touchpad_model.h"

namespace prosperobrowser::frontend {

class Ps5Touchpad {
 public:
  // Fills contacts with the fingers now on pad's touchpad (normalized), or
  // leaves it empty. pad may be null (no controller).
  void read(SDL_GameController* pad, std::uint64_t now_ms, std::vector<core::TouchContact>& contacts);
  // Pad height / width once known (see core::TouchpadModel::set_aspect()).
  float aspect() const { return static_cast<float>(height_) / static_cast<float>(width_); }
  // A controller came or went: find the pad handle again.
  void reset() { handle_ = -1; }

 private:
  bool resolve_handle(std::uint64_t now_ms);
  void read_resolution();

  int handle_ = -1;
  std::uint64_t next_resolve_ms_ = 0;
  int width_ = 1920;   // DualSense touch resolution (hid-playstation:
  int height_ = 1080;  // DS_TOUCHPAD_WIDTH x DS_TOUCHPAD_HEIGHT)
  bool resolution_read_ = false;
  bool resolution_from_info_ = false;
  bool source_logged_ = false;
  bool grow_logged_ = false;
};

}  // namespace prosperobrowser::frontend
