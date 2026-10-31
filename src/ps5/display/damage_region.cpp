// SPDX-License-Identifier: GPL-3.0-or-later
#include "damage_region.h"

#include <algorithm>

namespace prosperobrowser::display {

Rect intersect(const Rect& a, const Rect& b) {
  const int x0 = std::max(a.x, b.x);
  const int y0 = std::max(a.y, b.y);
  const int x1 = std::min(a.right(), b.right());
  const int y1 = std::min(a.bottom(), b.bottom());
  if (a.empty() || b.empty() || x1 <= x0 || y1 <= y0) {
    return Rect{};
  }
  return Rect{x0, y0, x1 - x0, y1 - y0};
}

Rect bounding(const Rect& a, const Rect& b) {
  if (a.empty()) {
    return b.empty() ? Rect{} : b;
  }
  if (b.empty()) {
    return a;
  }
  const int x0 = std::min(a.x, b.x);
  const int y0 = std::min(a.y, b.y);
  const int x1 = std::max(a.right(), b.right());
  const int y1 = std::max(a.bottom(), b.bottom());
  return Rect{x0, y0, x1 - x0, y1 - y0};
}

Region::Region(std::size_t max_rects) : max_rects_(std::max<std::size_t>(1, max_rects)) {}

void Region::add(const Rect& r) {
  if (r.empty()) {
    return;
  }
  for (const Rect& existing : rects_) {
    if (existing.contains(r)) {
      return;
    }
  }
  std::erase_if(rects_, [&r](const Rect& existing) { return r.contains(existing); });
  rects_.push_back(r);
  coalesce();
}

void Region::add(const Region& other) {
  for (const Rect& r : other.rects_) {
    add(r);
  }
}

void Region::clip(const Rect& bounds) {
  for (Rect& r : rects_) {
    r = intersect(r, bounds);
  }
  std::erase_if(rects_, [](const Rect& r) { return r.empty(); });
}

void Region::translate(int dx, int dy) {
  for (Rect& r : rects_) {
    r.x += dx;
    r.y += dy;
  }
}

Rect Region::bounds() const {
  Rect b;
  for (const Rect& r : rects_) {
    b = bounding(b, r);
  }
  return b;
}

std::uint64_t Region::area() const {
  std::uint64_t total = 0;
  for (const Rect& r : rects_) {
    total += r.area();
  }
  return total;
}

bool Region::covers(int px, int py) const {
  return std::any_of(rects_.begin(), rects_.end(), [px, py](const Rect& r) {
    return px >= r.x && px < r.right() && py >= r.y && py < r.bottom();
  });
}

void Region::coalesce() {
  // Greedy: merge the pair whose bounding rect wastes the least, while that
  // waste is small or there are too many rects. n stays tiny (<= max + 1).
  for (;;) {
    if (rects_.size() < 2) {
      return;
    }
    std::size_t best_i = 0;
    std::size_t best_j = 1;
    std::uint64_t best_waste = UINT64_MAX;
    bool best_cheap = false;
    for (std::size_t i = 0; i < rects_.size(); i++) {
      for (std::size_t j = i + 1; j < rects_.size(); j++) {
        const Rect& a = rects_[i];
        const Rect& b = rects_[j];
        const Rect box = bounding(a, b);
        const std::uint64_t covered = a.area() + b.area() - intersect(a, b).area();
        const std::uint64_t waste = box.area() - covered;
        if (waste < best_waste) {
          best_waste = waste;
          best_i = i;
          best_j = j;
          best_cheap = waste <= std::max(kMergeSlackPixels, box.area() / 8);
        }
      }
    }
    if (!best_cheap && rects_.size() <= max_rects_) {
      return;
    }
    const Rect merged = bounding(rects_[best_i], rects_[best_j]);
    rects_.erase(rects_.begin() + static_cast<std::ptrdiff_t>(best_j));
    rects_.erase(rects_.begin() + static_cast<std::ptrdiff_t>(best_i));
    // The merged rect may now contain others.
    std::erase_if(rects_, [&merged](const Rect& r) { return merged.contains(r); });
    rects_.push_back(merged);
  }
}

}  // namespace prosperobrowser::display
