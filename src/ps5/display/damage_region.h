// SPDX-License-Identifier: GPL-3.0-or-later
//
// Damage tracking of the UI compositor: integer rectangles and a small,
// self-coalescing list of them. The frame pipeline paints, composes and
// presents only what a Region covers, so its rect count is capped and near
// rects are merged when that wastes little area: few large rects are cheaper
// to process than many small ones, and a full-screen bound is never needed
// for two distant small updates.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace prosperoweb::display {

struct Rect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;

  bool empty() const { return w <= 0 || h <= 0; }
  int right() const { return x + w; }
  int bottom() const { return y + h; }
  std::uint64_t area() const {
    return empty() ? 0 : static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h);
  }
  // An empty rect is contained everywhere; nothing non-empty is in an empty one.
  bool contains(const Rect& o) const {
    return o.empty() || (!empty() && o.x >= x && o.y >= y && o.right() <= right() &&
                         o.bottom() <= bottom());
  }
  friend bool operator==(const Rect&, const Rect&) = default;
};

// The overlap of a and b (empty when they do not overlap).
Rect intersect(const Rect& a, const Rect& b);
// The smallest rect covering a and b (empty inputs are ignored).
Rect bounding(const Rect& a, const Rect& b);

class Region {
 public:
  static constexpr std::size_t kDefaultMaxRects = 8;
  // Two rects are merged when their bounding rect wastes at most this many
  // pixels, or an eighth of its area, over what the two cover.
  static constexpr std::uint64_t kMergeSlackPixels = 64 * 64;

  explicit Region(std::size_t max_rects = kDefaultMaxRects);

  // Adds r (ignored when empty) and re-coalesces.
  void add(const Rect& r);
  void add(const Region& other);
  // Drops everything outside bounds.
  void clip(const Rect& bounds);
  void translate(int dx, int dy);
  void clear() { rects_.clear(); }

  bool empty() const { return rects_.empty(); }
  const std::vector<Rect>& rects() const { return rects_; }
  std::size_t max_rects() const { return max_rects_; }
  // Bounding rect of all rects (empty for an empty region).
  Rect bounds() const;
  // Sum of the rect areas (rects may overlap: an upper bound of the
  // covered area).
  std::uint64_t area() const;
  // Whether point (px, py) is covered.
  bool covers(int px, int py) const;

 private:
  void coalesce();

  std::size_t max_rects_;
  std::vector<Rect> rects_;
};

}  // namespace prosperoweb::display
