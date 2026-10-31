// SPDX-License-Identifier: GPL-3.0-or-later
//
// From a page's view coordinates (what the WebProcess reports for a video
// box) to the UI window: the active view is drawn at y = content_top (below
// the chrome band, 0 when the chrome is hidden, e.g. in fullscreen), and a
// video may only cover the content area, never the band above it.
#pragma once

#include <vector>

#include "damage_region.h"

namespace prosperoweb::video {

// The part of the window the page occupies.
inline display::Rect content_area(int content_top, int width, int height) {
  return display::Rect{0, content_top, width, height - content_top};
}

// A view rect in window coordinates (not clipped).
inline display::Rect view_to_window(const display::Rect& r, int content_top) {
  return display::Rect{r.x, r.y + content_top, r.w, r.h};
}

// View-coordinate damage as window damage, clipped to the content area.
inline display::Region damage_to_window(const std::vector<display::Rect>& rects, int content_top,
                                        int width, int height) {
  display::Region region;
  const display::Rect area = content_area(content_top, width, height);
  for (const display::Rect& r : rects) {
    region.add(display::intersect(view_to_window(r, content_top), area));
  }
  return region;
}

}  // namespace prosperoweb::video
