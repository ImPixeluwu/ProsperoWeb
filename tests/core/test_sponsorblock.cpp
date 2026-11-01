// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "prosperoweb/core/sponsorblock.h"

using prosperoweb::core::sponsorblock::filter_enabled;
using prosperoweb::core::sponsorblock::Segment;
using prosperoweb::core::sponsorblock::seek_target;

TEST_SUITE("sponsorblock") {
  TEST_CASE("filter keeps only enabled categories") {
    std::vector<Segment> segs = {
        {10, 20, "sponsor"}, {30, 40, "intro"}, {50, 60, "selfpromo"}};
    auto kept = filter_enabled(segs, {"sponsor", "selfpromo"});
    REQUIRE(kept.size() == 2);
    CHECK(kept[0].category == "sponsor");
    CHECK(kept[1].category == "selfpromo");
  }

  TEST_CASE("inside a segment -> seek to its end") {
    std::vector<Segment> segs = {{10, 20, "sponsor"}};
    auto t = seek_target(12.0, segs);
    REQUIRE(t.has_value());
    CHECK(*t == doctest::Approx(20.0));
  }

  TEST_CASE("at the start boundary within epsilon -> skip") {
    std::vector<Segment> segs = {{10, 20, "sponsor"}};
    auto t = seek_target(9.9, segs, 0.3);
    REQUIRE(t.has_value());
    CHECK(*t == doctest::Approx(20.0));
  }

  TEST_CASE("outside any segment -> no skip") {
    std::vector<Segment> segs = {{10, 20, "sponsor"}};
    CHECK_FALSE(seek_target(5.0, segs).has_value());
    CHECK_FALSE(seek_target(25.0, segs).has_value());
  }

  TEST_CASE("near the end of a segment -> no skip (already past)") {
    std::vector<Segment> segs = {{10, 20, "sponsor"}};
    // Within epsilon of the end: treated as past the segment, no seek.
    CHECK_FALSE(seek_target(19.9, segs, 0.3).has_value());
  }

  TEST_CASE("chained overlapping segments collapse to the furthest end") {
    std::vector<Segment> segs = {{10, 20, "sponsor"}, {19, 35, "selfpromo"}, {34, 40, "intro"}};
    auto t = seek_target(12.0, segs);
    REQUIRE(t.has_value());
    CHECK(*t == doctest::Approx(40.0));
  }

  TEST_CASE("zero-length or inverted segments are ignored") {
    std::vector<Segment> segs = {{10, 10, "sponsor"}, {30, 20, "sponsor"}};
    CHECK_FALSE(seek_target(10.0, segs).has_value());
    CHECK_FALSE(seek_target(25.0, segs).has_value());
  }
}
