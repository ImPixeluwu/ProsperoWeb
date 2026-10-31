// SPDX-License-Identifier: GPL-3.0-or-later
// Built when SDL2 is not available: no physical controller support.
#include "gamepad.h"

namespace prosperobrowser::host {

struct Gamepad::Impl {};

Gamepad::Gamepad() : impl_(std::make_unique<Impl>()) {}
Gamepad::~Gamepad() = default;
bool Gamepad::connected() const { return false; }
void Gamepad::poll(const std::function<void(core::Button, bool)>& /*on_button*/) {}

}  // namespace prosperobrowser::host
