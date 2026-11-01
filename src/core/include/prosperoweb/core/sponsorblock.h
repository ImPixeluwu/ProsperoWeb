// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pure SponsorBlock segment-skip logic, shared in spirit with the bundled
// user script (data/addons/sponsorblock.js mirrors seek_target exactly). The
// C++ copy is what the host tests exercise; keeping both to the same rules is
// a maintenance contract, not enforced by the build.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace prosperoweb::core::sponsorblock {

// A skippable segment from the SponsorBlock API: [start, end] seconds.
struct Segment {
  double start = 0.0;
  double end = 0.0;
  std::string category;
};

// Keep only segments whose category is in `enabled`.
std::vector<Segment> filter_enabled(const std::vector<Segment>& segments,
                                     const std::vector<std::string>& enabled);

// Given the current playback time and the (already category-filtered)
// segments, return where the player should seek to, or nullopt for no skip.
//
// A segment skips when current_time is at or just after its start and before
// its end (a small lead-in tolerance catches the boundary frame). The seek
// target is the segment end; overlapping segments collapse to the furthest
// end reachable by chaining, so a seek never lands inside another segment.
std::optional<double> seek_target(double current_time,
                                   const std::vector<Segment>& segments,
                                   double epsilon = 0.3);

}  // namespace prosperoweb::core::sponsorblock
