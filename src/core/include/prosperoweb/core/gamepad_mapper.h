// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "prosperoweb/core/gamepad.h"

namespace prosperoweb::core {

// Maps button presses to actions using the default layout below. Actions
// fire on press; releases map to Action::None (key repeat for held buttons
// is handled by RepeatTimer in ShellController). The PS button is reserved by the
// system and always maps to Action::None.
//
// Page mode (pointer over content):
//   Cross=Click  Circle=Back  Triangle=FocusAddressBar  Square=OpenOsk
//   L1=PrevTab  R1=NextTab  L2=PageUp  R2=PageDown
//   D-pad=Scroll{Up,Down,Left,Right}  Options=OpenMenu  Share=Reload
//   L3=ToggleChrome  R3=ToggleFullscreen
//   Touchpad (the pad's button) = Click: the shell holds the mouse button
//   down while it is pressed, so press and slide drags. The pad's surface is
//   a trackpad (core::TouchpadModel: slide = pointer, tap = click, two
//   fingers = scroll). New tab: Options menu -> New tab, or "+" in the tab
//   strip (it used to be the touchpad button).
//
// Chrome mode (address bar / tab strip / menus):
//   D-pad=Navigate{Up,Down,Left,Right}  Cross=Activate
//   Circle=ToggleChrome (back to page)  Triangle=FocusAddressBar
//   Square=CloseTab  L1=PrevTab  R1=NextTab  Options=OpenMenu
//   Share=Reload  Touchpad=NewTab  R3=ToggleFullscreen
//
// Osk mode (on-screen keyboard):
//   D-pad=OskMove{Up,Down,Left,Right} (moves the key focus)
//   Cross=OskSelect  Circle=OskCancel  Options=OskConfirm
//   Square=OskBackspace (deletes before the caret)  Triangle=OskSpace
//   L1=OskCaretLeft  R1=OskCaretRight (move the text caret)
//   L2=OskShift  Touchpad=OskNextLayout (the on-screen layout key too)
//   D-pad, Square, L1 and R1 auto-repeat while held. Clearing the whole
//   text is only the on-screen Clear key, never a long press.
class GamepadMapper {
 public:
  Action map(Button button, bool pressed, Mode mode) const;

 private:
  static Action map_page(Button button);
  static Action map_chrome(Button button);
  static Action map_osk(Button button);
};

}  // namespace prosperoweb::core
