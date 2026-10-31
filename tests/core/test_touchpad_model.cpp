// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <vector>

#include "prosperoweb/core/touchpad_model.h"

using prosperoweb::core::TouchContact;
using prosperoweb::core::TouchpadModel;

namespace {

constexpr float kAspect = TouchpadModel::kDefaultAspect;

// Slides finger id from (x0, y0) by (dx, dy) in `steps` moves `step_ms` apart,
// starting at t; returns the time of the last move.
std::uint64_t slide(TouchpadModel& m, int id, float x0, float y0, float dx, float dy, int steps,
                    std::uint64_t step_ms, std::uint64_t t) {
  for (int i = 1; i <= steps; ++i) {
    t += step_ms;
    const float k = static_cast<float>(i) / static_cast<float>(steps);
    m.finger_move(id, x0 + dx * k, y0 + dy * k, t);
  }
  return t;
}

}  // namespace

TEST_SUITE("touchpad_model") {
  TEST_CASE("a slow one-finger slide moves the pointer at the base gain") {
    TouchpadModel m;
    m.finger_down(0, 0.4f, 0.5f, 1000);
    const auto t = slide(m, 0, 0.4f, 0.5f, 0.1f, 0.0f, 10, 100, 1000);  // 0.1 pad widths/s
    m.finger_up(0, t + 10);
    const auto out = m.take();
    CHECK(out.pointer.x == doctest::Approx(0.1f * TouchpadModel::kPointerGain).epsilon(0.01));
    CHECK(out.pointer.y == doctest::Approx(0.0f));
    CHECK(out.clicks == 0);
    CHECK(out.scroll.y == doctest::Approx(0.0f));
  }

  TEST_CASE("vertical travel is scaled by the pad's aspect ratio") {
    TouchpadModel m;
    m.finger_down(0, 0.5f, 0.3f, 0);
    slide(m, 0, 0.5f, 0.3f, 0.0f, 0.1f, 10, 100, 0);
    const auto out = m.take();
    CHECK(out.pointer.x == doctest::Approx(0.0f));
    CHECK(out.pointer.y ==
          doctest::Approx(0.1f * kAspect * TouchpadModel::kPointerGain).epsilon(0.01));
  }

  TEST_CASE("a fast flick is accelerated up to kMaxAcceleration") {
    TouchpadModel slow;
    slow.finger_down(0, 0.2f, 0.5f, 0);
    slide(slow, 0, 0.2f, 0.5f, 0.3f, 0.0f, 30, 100, 0);
    TouchpadModel fast;
    fast.finger_down(0, 0.2f, 0.5f, 0);
    slide(fast, 0, 0.2f, 0.5f, 0.3f, 0.0f, 3, 16, 0);  // ~6 pad widths/s
    const float s = slow.take().pointer.x;
    const float f = fast.take().pointer.x;
    CHECK(f > s * 2.5f);
    CHECK(f == doctest::Approx(0.3f * TouchpadModel::kPointerGain *
                               TouchpadModel::kMaxAcceleration).epsilon(0.01));
  }

  TEST_CASE("leftward motion moves the pointer left") {
    TouchpadModel m;
    m.finger_down(0, 0.8f, 0.5f, 0);
    slide(m, 0, 0.8f, 0.5f, -0.2f, 0.0f, 10, 50, 0);
    CHECK(m.take().pointer.x < -100.0f);
  }

  TEST_CASE("a quick still tap clicks and does not move the pointer") {
    TouchpadModel m;
    m.finger_down(3, 0.5f, 0.5f, 0);
    m.finger_move(3, 0.505f, 0.502f, 50);  // jitter under the slop
    m.finger_up(3, 120);
    const auto out = m.take();
    CHECK(out.clicks == 1);
    CHECK(out.pointer.x == doctest::Approx(0.0f));
    CHECK(out.pointer.y == doctest::Approx(0.0f));
  }

  TEST_CASE("a long touch is not a tap") {
    TouchpadModel m;
    m.finger_down(0, 0.5f, 0.5f, 0);
    m.finger_up(0, TouchpadModel::kTapMaxMs + 50);
    CHECK(m.take().clicks == 0);
  }

  TEST_CASE("a quick touch that moved past the slop is a move, not a tap") {
    TouchpadModel m;
    m.finger_down(0, 0.5f, 0.5f, 0);
    m.finger_move(0, 0.54f, 0.5f, 60);
    m.finger_up(0, 100);
    const auto out = m.take();
    CHECK(out.clicks == 0);
    CHECK(out.pointer.x > 0.0f);
  }

  TEST_CASE("a still finger held past the tap time starts moving without a jump") {
    TouchpadModel m;
    m.finger_down(0, 0.5f, 0.5f, 0);
    m.finger_move(0, 0.501f, 0.5f, 400);
    const float first = m.take().pointer.x;
    CHECK(first >= 0.0f);
    CHECK(first < 2.0f);
    m.finger_move(0, 0.511f, 0.5f, 500);
    CHECK(m.take().pointer.x == doctest::Approx(0.01f * TouchpadModel::kPointerGain).epsilon(0.01));
  }

  TEST_CASE("a two-finger tap does not click") {
    TouchpadModel m;
    m.finger_down(0, 0.4f, 0.5f, 0);
    m.finger_down(1, 0.6f, 0.5f, 10);
    m.finger_up(0, 80);
    m.finger_up(1, 90);
    CHECK(m.take().clicks == 0);
  }

  TEST_CASE("two fingers scroll in the natural direction and leave the pointer") {
    TouchpadModel m;
    m.finger_down(0, 0.4f, 0.6f, 0);
    m.finger_down(1, 0.6f, 0.6f, 0);
    CHECK(m.multi_finger());
    // Both fingers up by 0.2 of the pad height: the content follows them up,
    // which scrolls the page down.
    for (int i = 1; i <= 10; ++i) {
      const float y = 0.6f - 0.02f * static_cast<float>(i);
      m.finger_move(0, 0.4f, y, static_cast<std::uint64_t>(i) * 16);
      m.finger_move(1, 0.6f, y, static_cast<std::uint64_t>(i) * 16);
    }
    const auto out = m.take();
    CHECK(out.scroll.y == doctest::Approx(0.2f * kAspect * TouchpadModel::kScrollGain).epsilon(0.01));
    CHECK(out.scroll.x == doctest::Approx(0.0f));
    CHECK(out.pointer.x == doctest::Approx(0.0f));
    CHECK(out.pointer.y == doctest::Approx(0.0f));
  }

  TEST_CASE("a two-finger swipe to the right scrolls left") {
    TouchpadModel m;
    m.finger_down(0, 0.3f, 0.4f, 0);
    m.finger_down(1, 0.3f, 0.6f, 0);
    m.finger_move(0, 0.4f, 0.4f, 16);
    m.finger_move(1, 0.4f, 0.6f, 16);
    const auto out = m.take();
    CHECK(out.scroll.x == doctest::Approx(-0.1f * TouchpadModel::kScrollGain).epsilon(0.01));
    CHECK(out.scroll.y == doctest::Approx(0.0f));
  }

  TEST_CASE("after a scroll the remaining finger does not move the pointer") {
    TouchpadModel m;
    m.finger_down(0, 0.4f, 0.5f, 0);
    m.finger_down(1, 0.6f, 0.5f, 0);
    m.finger_up(1, 100);
    slide(m, 0, 0.4f, 0.5f, 0.2f, 0.0f, 5, 20, 100);
    CHECK(m.take().pointer.x == doctest::Approx(0.0f));
    m.finger_up(0, 300);
    // A fresh touch moves it again.
    m.finger_down(0, 0.4f, 0.5f, 400);
    slide(m, 0, 0.4f, 0.5f, 0.2f, 0.0f, 5, 20, 400);
    CHECK(m.take().pointer.x > 0.0f);
  }

  TEST_CASE("pressing the pad is not also a tap, and drags without slop") {
    TouchpadModel m;
    m.finger_down(0, 0.5f, 0.5f, 0);
    m.set_pressed(true);
    m.finger_move(0, 0.505f, 0.5f, 30);  // under the slop, but held: drag
    CHECK(m.take().pointer.x > 0.0f);
    m.set_pressed(false);
    m.finger_up(0, 100);
    CHECK(m.take().clicks == 0);
    // The next touch taps normally again.
    m.finger_down(0, 0.5f, 0.5f, 1000);
    m.finger_up(0, 1050);
    CHECK(m.take().clicks == 1);
  }

  TEST_CASE("snapshots become finger events") {
    TouchpadModel m;
    std::vector<TouchContact> one{{7, 0.5f, 0.5f}};
    m.update(one, 0);
    CHECK(m.fingers() == 1);
    m.update(one, 16);  // unchanged: nothing
    m.update({}, 60);   // lifted quickly: a tap
    CHECK(m.fingers() == 0);
    CHECK(m.take().clicks == 1);

    m.update(one, 1000);
    std::vector<TouchContact> moved{{7, 0.6f, 0.5f}};
    m.update(moved, 1100);
    CHECK(m.take().pointer.x > 0.0f);
    std::vector<TouchContact> two{{7, 0.6f, 0.5f}, {8, 0.7f, 0.5f}};
    m.update(two, 1116);
    CHECK(m.fingers() == 2);
    CHECK(m.multi_finger());
    m.update({}, 1200);
    CHECK(m.fingers() == 0);
  }

  TEST_CASE("a new finger id in one snapshot ends the old touch first") {
    TouchpadModel m;
    std::vector<TouchContact> a{{1, 0.5f, 0.5f}};
    std::vector<TouchContact> b{{2, 0.9f, 0.9f}};
    m.update(a, 0);
    m.update(b, 50);  // finger 1 lifted (a tap), finger 2 landed elsewhere
    const auto out = m.take();
    CHECK(out.clicks == 1);
    CHECK(out.pointer.x == doctest::Approx(0.0f));  // no jump to the new finger
  }

  TEST_CASE("take() clears the output and reset() drops fingers without a tap") {
    TouchpadModel m;
    m.finger_down(0, 0.2f, 0.5f, 0);
    slide(m, 0, 0.2f, 0.5f, 0.3f, 0.0f, 5, 50, 0);
    CHECK_FALSE(m.take().empty());
    CHECK(m.take().empty());
    m.finger_down(1, 0.5f, 0.5f, 1000);
    m.reset();
    CHECK(m.fingers() == 0);
    m.finger_up(1, 1050);
    CHECK(m.take().clicks == 0);
  }

  TEST_CASE("set_aspect changes the vertical scale") {
    TouchpadModel m(1.0f);
    CHECK(m.aspect() == doctest::Approx(1.0f));
    m.set_aspect(0.5f);
    m.finger_down(0, 0.5f, 0.2f, 0);
    slide(m, 0, 0.5f, 0.2f, 0.0f, 0.2f, 10, 100, 0);
    CHECK(m.take().pointer.y == doctest::Approx(0.1f * TouchpadModel::kPointerGain).epsilon(0.01));
  }
}
