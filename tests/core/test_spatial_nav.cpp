// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <vector>

#include "prosperoweb/core/spatial_nav.h"

using namespace prosperoweb::core;

namespace {

// A 3x3 grid of 100x40 links with 50 px gaps, starting at (100, 100):
//   0 1 2
//   3 4 5
//   6 7 8
std::vector<NavRect> grid() {
  std::vector<NavRect> out;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      out.push_back({100.0f + col * 150.0f, 100.0f + row * 90.0f, 100.0f, 40.0f});
    }
  }
  return out;
}

Vec2 centre(const NavRect& r) { return {r.x + r.width / 2.0f, r.y + r.height / 2.0f}; }

}  // namespace

TEST_SUITE("spatial_nav") {
  TEST_CASE("direction names round trip") {
    for (auto d : {NavDirection::Up, NavDirection::Down, NavDirection::Left, NavDirection::Right}) {
      CHECK(direction_from_name(direction_name(d)) == d);
    }
    CHECK_FALSE(direction_from_name("sideways").has_value());
  }

  TEST_CASE("moves to the neighbour in each direction from the middle") {
    const auto g = grid();
    const Vec2 from = centre(g[4]);
    CHECK(pick_spatial_target(from, NavDirection::Up, g) == 1u);
    CHECK(pick_spatial_target(from, NavDirection::Down, g) == 7u);
    CHECK(pick_spatial_target(from, NavDirection::Left, g) == 3u);
    CHECK(pick_spatial_target(from, NavDirection::Right, g) == 5u);
  }

  TEST_CASE("nothing beyond the edge") {
    const auto g = grid();
    CHECK_FALSE(pick_spatial_target(centre(g[0]), NavDirection::Up, g).has_value());
    CHECK_FALSE(pick_spatial_target(centre(g[0]), NavDirection::Left, g).has_value());
    CHECK_FALSE(pick_spatial_target(centre(g[8]), NavDirection::Down, g).has_value());
    CHECK_FALSE(pick_spatial_target(centre(g[8]), NavDirection::Right, g).has_value());
    CHECK_FALSE(pick_spatial_target({10, 10}, NavDirection::Down, std::vector<NavRect>{})
                    .has_value());
  }

  TEST_CASE("the same row beats a closer diagonal") {
    // From (0, 100): a link far away on the same line and a nearer one
    // well below it.
    const std::vector<NavRect> t = {{400, 90, 80, 20}, {60, 200, 80, 20}};
    CHECK(pick_spatial_target({0, 100}, NavDirection::Right, t) == 0u);
  }

  TEST_CASE("down a column of links of different widths goes to the next one") {
    // From the centre of a long link (x 0..600), the next, short link below
    // beats a far one that happens to sit right under the pointer.
    const std::vector<NavRect> t = {{0, 0, 600, 30}, {0, 40, 150, 30}, {280, 700, 60, 30}};
    CHECK(pick_spatial_target({300, 15}, NavDirection::Down, t) == 1u);
  }

  TEST_CASE("the element under the pointer is skipped") {
    const std::vector<NavRect> t = {{0, 0, 200, 40}, {0, 60, 200, 40}};
    CHECK(pick_spatial_target({100, 20}, NavDirection::Down, t) == 1u);
    CHECK_FALSE(pick_spatial_target({100, 20}, NavDirection::Up, t).has_value());
  }

  TEST_CASE("a target must be past the pointer, not beside it") {
    // A tall element whose centre is level with the pointer is not "below".
    const std::vector<NavRect> t = {{300, 0, 50, 400}};
    CHECK_FALSE(pick_spatial_target({100, 200}, NavDirection::Down, t).has_value());
    CHECK(pick_spatial_target({100, 200}, NavDirection::Right, t) == 0u);
  }

  TEST_CASE("empty rectangles are ignored") {
    const std::vector<NavRect> t = {{0, 100, 0, 0}, {0, 200, 50, 20}};
    CHECK(pick_spatial_target({10, 10}, NavDirection::Down, t) == 1u);
  }

  TEST_CASE("the page script reports targets for its direction") {
    const std::string js = spatial_targets_script(NavDirection::Left);
    CHECK(js.find("spatial_targets") != std::string::npos);
    CHECK(js.find("messageHandlers.prosperoweb.postMessage") != std::string::npos);
    CHECK(js.ends_with("})('left');"));
  }
}
