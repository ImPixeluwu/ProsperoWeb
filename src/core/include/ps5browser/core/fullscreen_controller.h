// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace ps5browser::core {

// Tracks whether browser chrome is shown and whether the page holds the
// HTML5 Fullscreen API.
class FullscreenController {
 public:
  enum class State : std::uint8_t {
    Normal,             // chrome visible
    ChromeHidden,       // user hid the chrome (R3 / L3)
    ElementFullscreen,  // page called requestFullscreen()
  };

  // Side effects the frontend must perform.
  enum class Effect : std::uint8_t {
    None,
    RequestExitElementFullscreen,  // tell WebKit to exit element fullscreen
  };

  State state() const { return state_; }
  bool chrome_visible() const { return state_ == State::Normal; }

  // Normal <-> ChromeHidden. Ignored during element fullscreen.
  Effect toggle_chrome();
  // WebKit entered element fullscreen.
  Effect page_entered_fullscreen();
  // WebKit left element fullscreen (page-initiated or after our request);
  // restores the state that was active before entering.
  Effect page_exited_fullscreen();
  // Circle / ExitFullscreen: leaves any fullscreen-like state for Normal.
  Effect user_exit();

 private:
  State state_ = State::Normal;
  State before_element_ = State::Normal;
};

}  // namespace ps5browser::core
