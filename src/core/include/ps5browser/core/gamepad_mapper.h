// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ps5browser/core/gamepad.h"

namespace ps5browser::core {

// Maps button presses to actions using the default layout below. Actions
// fire on press; releases map to Action::None (key repeat for scrolling is
// handled by RepeatTimer in the frontend). The PS button is reserved by the
// system and always maps to Action::None.
//
// Page mode (pointer over content):
//   Cross=Click  Circle=Back  Triangle=FocusAddressBar  Square=OpenOsk
//   L1=PrevTab  R1=NextTab  L2=PageUp  R2=PageDown
//   D-pad=Scroll{Up,Down,Left,Right}  Options=OpenMenu  Share=Reload
//   L3=ToggleChrome  R3=ToggleFullscreen  Touchpad=NewTab
//
// Chrome mode (address bar / tab strip / menus):
//   D-pad=Navigate{Up,Down,Left,Right}  Cross=Activate
//   Circle=ToggleChrome (back to page)  Triangle=FocusAddressBar
//   Square=CloseTab  L1=PrevTab  R1=NextTab  Options=OpenMenu
//   Share=Reload  Touchpad=NewTab  R3=ToggleFullscreen
//
// Osk mode (on-screen keyboard):
//   D-pad=OskMove{Up,Down,Left,Right}  Cross=OskSelect  Circle=OskCancel
//   Options=OskConfirm  Square=OskBackspace  Triangle=OskSpace
//   L2=OskShift  R1=OskNextLayout
class GamepadMapper {
 public:
  Action map(Button button, bool pressed, Mode mode) const;

 private:
  static Action map_page(Button button);
  static Action map_chrome(Button button);
  static Action map_osk(Button button);
};

}  // namespace ps5browser::core
