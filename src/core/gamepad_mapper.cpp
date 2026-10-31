// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps5browser/core/gamepad_mapper.h"

namespace ps5browser::core {

Action GamepadMapper::map(Button button, bool pressed, Mode mode) const {
  if (!pressed || button == Button::PS) {
    return Action::None;
  }
  switch (mode) {
    case Mode::Page:
      return map_page(button);
    case Mode::Chrome:
      return map_chrome(button);
    case Mode::Osk:
      return map_osk(button);
  }
  return Action::None;
}

Action GamepadMapper::map_page(Button button) {
  switch (button) {
    case Button::Cross: return Action::Click;
    case Button::Circle: return Action::Back;
    case Button::Triangle: return Action::FocusAddressBar;
    case Button::Square: return Action::OpenOsk;
    case Button::L1: return Action::PrevTab;
    case Button::R1: return Action::NextTab;
    case Button::L2: return Action::PageUp;
    case Button::R2: return Action::PageDown;
    case Button::L3: return Action::ToggleChrome;
    case Button::R3: return Action::ToggleFullscreen;
    case Button::Up: return Action::ScrollUp;
    case Button::Down: return Action::ScrollDown;
    case Button::Left: return Action::ScrollLeft;
    case Button::Right: return Action::ScrollRight;
    case Button::Options: return Action::OpenMenu;
    case Button::Share: return Action::Reload;
    case Button::Touchpad: return Action::NewTab;
    case Button::PS: return Action::None;
  }
  return Action::None;
}

Action GamepadMapper::map_chrome(Button button) {
  switch (button) {
    case Button::Cross: return Action::Activate;
    case Button::Circle: return Action::ToggleChrome;
    case Button::Triangle: return Action::FocusAddressBar;
    case Button::Square: return Action::CloseTab;
    case Button::L1: return Action::PrevTab;
    case Button::R1: return Action::NextTab;
    case Button::R3: return Action::ToggleFullscreen;
    case Button::Up: return Action::NavigateUp;
    case Button::Down: return Action::NavigateDown;
    case Button::Left: return Action::NavigateLeft;
    case Button::Right: return Action::NavigateRight;
    case Button::Options: return Action::OpenMenu;
    case Button::Share: return Action::Reload;
    case Button::Touchpad: return Action::NewTab;
    case Button::L2:
    case Button::R2:
    case Button::L3:
    case Button::PS: return Action::None;
  }
  return Action::None;
}

Action GamepadMapper::map_osk(Button button) {
  switch (button) {
    case Button::Cross: return Action::OskSelect;
    case Button::Circle: return Action::OskCancel;
    case Button::Options: return Action::OskConfirm;
    case Button::Square: return Action::OskBackspace;
    case Button::Triangle: return Action::OskSpace;
    case Button::L2: return Action::OskShift;
    case Button::L1: return Action::OskCaretLeft;
    case Button::R1: return Action::OskCaretRight;
    case Button::Touchpad: return Action::OskNextLayout;
    case Button::Up: return Action::OskMoveUp;
    case Button::Down: return Action::OskMoveDown;
    case Button::Left: return Action::OskMoveLeft;
    case Button::Right: return Action::OskMoveRight;
    case Button::R2:
    case Button::L3:
    case Button::R3:
    case Button::Share:
    case Button::PS: return Action::None;
  }
  return Action::None;
}

}  // namespace ps5browser::core
