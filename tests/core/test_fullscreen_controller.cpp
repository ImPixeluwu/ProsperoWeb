// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "ps5browser/core/fullscreen_controller.h"

using FC = ps5browser::core::FullscreenController;

TEST_SUITE("fullscreen") {
  TEST_CASE("toggle chrome") {
    FC fc;
    CHECK(fc.state() == FC::State::Normal);
    CHECK(fc.chrome_visible());
    fc.toggle_chrome();
    CHECK(fc.state() == FC::State::ChromeHidden);
    CHECK_FALSE(fc.chrome_visible());
    fc.toggle_chrome();
    CHECK(fc.state() == FC::State::Normal);
  }

  TEST_CASE("element fullscreen, exited by the page") {
    FC fc;
    fc.toggle_chrome();
    fc.page_entered_fullscreen();
    CHECK(fc.state() == FC::State::ElementFullscreen);
    CHECK_FALSE(fc.chrome_visible());
    fc.toggle_chrome();  // ignored while the page is fullscreen
    CHECK(fc.state() == FC::State::ElementFullscreen);
    fc.page_exited_fullscreen();
    CHECK(fc.state() == FC::State::ChromeHidden);  // restored
  }

  TEST_CASE("user exit") {
    FC fc;
    fc.page_entered_fullscreen();
    CHECK(fc.user_exit() == FC::Effect::RequestExitElementFullscreen);
    CHECK(fc.state() == FC::State::Normal);
    CHECK(fc.page_exited_fullscreen() == FC::Effect::None);  // WebKit's ack
    CHECK(fc.state() == FC::State::Normal);

    fc.toggle_chrome();
    CHECK(fc.user_exit() == FC::Effect::None);
    CHECK(fc.state() == FC::State::Normal);
    CHECK(fc.user_exit() == FC::Effect::None);
  }
}
