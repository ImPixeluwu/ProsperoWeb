// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cmath>

#include "prosperoweb/core/pointer_model.h"

using prosperoweb::core::PointerModel;
using prosperoweb::core::Vec2;

TEST_SUITE("pointer_model") {
  TEST_CASE("starts centred") {
    PointerModel p(1920, 1080);
    CHECK(p.position().x == doctest::Approx(960));
    CHECK(p.position().y == doctest::Approx(540));
  }

  TEST_CASE("dead zone") {
    PointerModel p(1920, 1080);
    const Vec2 d = p.update(0.1f, -0.1f, 1.0f);  // |v| ~ 0.141 < 0.15
    CHECK(d.x == doctest::Approx(0));
    CHECK(d.y == doctest::Approx(0));
    const Vec2 v = PointerModel::velocity(0.15f, 0.0f, PointerModel::kMaxSpeed);
    CHECK(v.x == doctest::Approx(0));
    CHECK(PointerModel::velocity(0.3f, 0.0f, PointerModel::kMaxSpeed).x > 0.0f);
  }

  TEST_CASE("acceleration is quadratic and capped") {
    const float half = PointerModel::velocity(0.575f, 0.0f, PointerModel::kMaxSpeed).x;
    const float full = PointerModel::velocity(1.0f, 0.0f, PointerModel::kMaxSpeed).x;
    CHECK(full == doctest::Approx(PointerModel::kMaxSpeed));
    CHECK(half == doctest::Approx(PointerModel::kMaxSpeed / 4.0f));  // t = 0.5
    // Diagonal beyond the unit circle is still capped.
    const Vec2 diag = PointerModel::velocity(1.0f, 1.0f, PointerModel::kMaxSpeed);
    CHECK(std::sqrt(diag.x * diag.x + diag.y * diag.y) == doctest::Approx(PointerModel::kMaxSpeed));
    // Direction is preserved.
    CHECK(PointerModel::velocity(-1.0f, 0.0f, PointerModel::kMaxSpeed).x < 0.0f);
  }

  TEST_CASE("movement over time and clamping to the viewport") {
    PointerModel p(1920, 1080);
    const Vec2 d = p.update(1.0f, 0.0f, 0.1f);
    CHECK(d.x == doctest::Approx(180.0f));
    CHECK(p.position().x == doctest::Approx(1140.0f));
    p.update(1.0f, 1.0f, 10.0f);
    CHECK(p.position().x == doctest::Approx(1919.0f));
    CHECK(p.position().y == doctest::Approx(1079.0f));
    p.update(-1.0f, -1.0f, 10.0f);
    CHECK(p.position().x == doctest::Approx(0.0f));
    CHECK(p.position().y == doctest::Approx(0.0f));
    p.set_position({5000, 5000});
    CHECK(p.position().x == doctest::Approx(1919.0f));
    p.set_viewport(1280, 720);
    CHECK(p.position().y == doctest::Approx(719.0f));
    CHECK(p.update(1.0f, 0.0f, 0.0f).x == doctest::Approx(0.0f));
  }

  TEST_CASE("right stick scroll") {
    const Vec2 s = PointerModel::scroll(0.0f, 1.0f, 0.5f);
    CHECK(s.x == doctest::Approx(0.0f));
    CHECK(s.y == doctest::Approx(PointerModel::kMaxScrollSpeed * 0.5f));
    CHECK(PointerModel::scroll(0.05f, 0.05f, 1.0f).y == doctest::Approx(0.0f));
  }
}
