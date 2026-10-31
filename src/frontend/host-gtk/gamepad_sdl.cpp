// SPDX-License-Identifier: GPL-3.0-or-later
#include "gamepad.h"

#include <SDL.h>

#include <cstdio>

namespace prosperobrowser::host {

namespace {

constexpr Sint16 kTriggerThreshold = 16000;

bool map_button(Uint8 button, core::Button& out) {
  using core::Button;
  switch (button) {
    case SDL_CONTROLLER_BUTTON_A: out = Button::Cross; return true;
    case SDL_CONTROLLER_BUTTON_B: out = Button::Circle; return true;
    case SDL_CONTROLLER_BUTTON_X: out = Button::Square; return true;
    case SDL_CONTROLLER_BUTTON_Y: out = Button::Triangle; return true;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: out = Button::L1; return true;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: out = Button::R1; return true;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: out = Button::L3; return true;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: out = Button::R3; return true;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: out = Button::Up; return true;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: out = Button::Down; return true;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: out = Button::Left; return true;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: out = Button::Right; return true;
    case SDL_CONTROLLER_BUTTON_START: out = Button::Options; return true;
    case SDL_CONTROLLER_BUTTON_BACK: out = Button::Share; return true;
    case SDL_CONTROLLER_BUTTON_TOUCHPAD: out = Button::Touchpad; return true;
    case SDL_CONTROLLER_BUTTON_GUIDE: out = Button::PS; return true;
    default: return false;
  }
}

float axis(SDL_GameController* c, SDL_GameControllerAxis a) {
  const float v = static_cast<float>(SDL_GameControllerGetAxis(c, a)) / 32767.0f;
  return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
}

}  // namespace

struct Gamepad::Impl {
  bool initialized = false;
  SDL_GameController* controller = nullptr;
  bool l2 = false;
  bool r2 = false;
};

Gamepad::Gamepad() : impl_(std::make_unique<Impl>()) {
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
  if (SDL_Init(SDL_INIT_GAMECONTROLLER) == 0) {
    impl_->initialized = true;
  } else {
    std::fprintf(stderr, "prosperobrowser-host: SDL game controller init failed: %s\n",
                 SDL_GetError());
  }
}

Gamepad::~Gamepad() {
  if (impl_->controller) {
    SDL_GameControllerClose(impl_->controller);
  }
  if (impl_->initialized) {
    SDL_Quit();
  }
}

bool Gamepad::connected() const { return impl_->controller != nullptr; }

void Gamepad::poll(const std::function<void(core::Button, bool)>& on_button) {
  if (!impl_->initialized) {
    return;
  }
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
      case SDL_CONTROLLERDEVICEADDED:
        if (!impl_->controller) {
          impl_->controller = SDL_GameControllerOpen(event.cdevice.which);
          if (impl_->controller) {
            std::fprintf(stderr, "prosperobrowser-host: controller connected: %s\n",
                         SDL_GameControllerName(impl_->controller));
          }
        }
        break;
      case SDL_CONTROLLERDEVICEREMOVED:
        if (impl_->controller &&
            SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(impl_->controller)) ==
                event.cdevice.which) {
          SDL_GameControllerClose(impl_->controller);
          impl_->controller = nullptr;
        }
        break;
      case SDL_CONTROLLERBUTTONDOWN:
      case SDL_CONTROLLERBUTTONUP: {
        core::Button b;
        if (map_button(event.cbutton.button, b)) {
          on_button(b, event.type == SDL_CONTROLLERBUTTONDOWN);
        }
        break;
      }
      default: break;
    }
  }
  auto* c = impl_->controller;
  if (!c) {
    for (float& s : sticks_) s = 0.0f;
    return;
  }
  sticks_[0] = axis(c, SDL_CONTROLLER_AXIS_LEFTX);
  sticks_[1] = axis(c, SDL_CONTROLLER_AXIS_LEFTY);
  sticks_[2] = axis(c, SDL_CONTROLLER_AXIS_RIGHTX);
  sticks_[3] = axis(c, SDL_CONTROLLER_AXIS_RIGHTY);
  // Triggers are axes in SDL; turn them into L2/R2 presses.
  const bool l2 = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kTriggerThreshold;
  const bool r2 = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kTriggerThreshold;
  if (l2 != impl_->l2) {
    impl_->l2 = l2;
    on_button(core::Button::L2, l2);
  }
  if (r2 != impl_->r2) {
    impl_->r2 = r2;
    on_button(core::Button::R2, r2);
  }
}

}  // namespace prosperobrowser::host
