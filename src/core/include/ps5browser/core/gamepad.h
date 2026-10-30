// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace ps5browser::core {

// DualSense buttons as seen by the frontend.
enum class Button : std::uint8_t {
  Cross,
  Circle,
  Square,
  Triangle,
  L1,
  R1,
  L2,
  R2,
  L3,
  R3,
  Up,
  Down,
  Left,
  Right,
  Options,
  Share,
  Touchpad,
  PS,
};

// Where input focus currently is.
enum class Mode : std::uint8_t {
  Page,    // pointer over web content
  Chrome,  // focus in address bar / tab strip / menus
  Osk,     // on-screen keyboard open
};

// Semantic actions the frontend executes. Some are only reachable from the
// menu (Forward, Stop, Zoom*, ToggleBookmark, ShowHistory, Quit) and have no
// default button binding.
enum class Action : std::uint8_t {
  None,
  // Page navigation
  Click,
  Back,
  Forward,
  Reload,
  Stop,
  ScrollUp,
  ScrollDown,
  ScrollLeft,
  ScrollRight,
  PageUp,
  PageDown,
  ZoomIn,
  ZoomOut,
  // Browser chrome
  ToggleChrome,
  FocusAddressBar,
  NewTab,
  CloseTab,
  NextTab,
  PrevTab,
  ToggleFullscreen,
  ExitFullscreen,
  OpenMenu,
  ToggleBookmark,
  ShowHistory,
  Quit,
  // Focus navigation inside chrome (tab strip, address bar, menus)
  NavigateUp,
  NavigateDown,
  NavigateLeft,
  NavigateRight,
  Activate,
  // On-screen keyboard
  OpenOsk,
  OskSelect,
  OskConfirm,
  OskCancel,
  OskBackspace,
  OskSpace,
  OskShift,
  OskNextLayout,
  OskMoveUp,
  OskMoveDown,
  OskMoveLeft,
  OskMoveRight,
};

}  // namespace ps5browser::core
