// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "prosperoweb/core/repeat_timer.h"

using prosperoweb::core::RepeatTimer;

TEST_SUITE("repeat_timer") {
  TEST_CASE("fires after the initial delay and then at the interval") {
    RepeatTimer t;
    CHECK(t.update(1000) == 0);  // not held
    CHECK(t.press());
    CHECK_FALSE(t.press());      // already held
    CHECK(t.update(299) == 0);
    CHECK(t.update(1) == 1);     // 300 ms
    CHECK(t.update(59) == 0);
    CHECK(t.update(1) == 1);     // 360 ms
    CHECK(t.update(120) == 2);   // 420, 480 ms
    t.release();
    CHECK_FALSE(t.held());
    CHECK(t.update(1000) == 0);
  }

  TEST_CASE("re-press restarts the delay") {
    RepeatTimer t(100, 50);
    t.press();
    CHECK(t.update(150) == 2);
    t.release();
    CHECK(t.press());
    CHECK(t.update(99) == 0);
    CHECK(t.update(1) == 1);
  }
}
