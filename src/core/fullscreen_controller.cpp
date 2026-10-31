// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperobrowser/core/fullscreen_controller.h"

namespace prosperobrowser::core {

using Effect = FullscreenController::Effect;
using State = FullscreenController::State;

Effect FullscreenController::toggle_chrome() {
  if (state_ == State::Normal) {
    state_ = State::ChromeHidden;
  } else if (state_ == State::ChromeHidden) {
    state_ = State::Normal;
  }
  return Effect::None;
}

Effect FullscreenController::page_entered_fullscreen() {
  if (state_ != State::ElementFullscreen) {
    before_element_ = state_;
    state_ = State::ElementFullscreen;
  }
  return Effect::None;
}

Effect FullscreenController::page_exited_fullscreen() {
  if (state_ == State::ElementFullscreen) {
    state_ = before_element_;
  }
  return Effect::None;
}

Effect FullscreenController::user_exit() {
  const State previous = state_;
  state_ = State::Normal;
  before_element_ = State::Normal;
  return previous == State::ElementFullscreen ? Effect::RequestExitElementFullscreen
                                              : Effect::None;
}

}  // namespace prosperobrowser::core
