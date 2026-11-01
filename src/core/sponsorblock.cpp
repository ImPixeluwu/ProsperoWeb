// SPDX-License-Identifier: GPL-3.0-or-later
#include "prosperoweb/core/sponsorblock.h"

#include <algorithm>

namespace prosperoweb::core::sponsorblock {

std::vector<Segment> filter_enabled(const std::vector<Segment>& segments,
                                    const std::vector<std::string>& enabled) {
  std::vector<Segment> out;
  for (const auto& s : segments) {
    if (std::find(enabled.begin(), enabled.end(), s.category) != enabled.end()) {
      out.push_back(s);
    }
  }
  return out;
}

std::optional<double> seek_target(double current_time,
                                  const std::vector<Segment>& segments, double epsilon) {
  // Find a segment that contains (or just precedes) the playhead.
  std::optional<double> target;
  bool progressed = true;
  double t = current_time;
  while (progressed) {
    progressed = false;
    for (const auto& s : segments) {
      if (s.end <= s.start) {
        continue;
      }
      // Inside the segment, or within epsilon before its start (so the skip
      // fires on the boundary frame rather than one tick late).
      if (t >= s.start - epsilon && t < s.end - epsilon) {
        if (s.end > t) {
          t = s.end;
          target = t;
          progressed = true;  // chain into any segment that begins here
        }
      }
    }
  }
  return target;
}

}  // namespace prosperoweb::core::sponsorblock
