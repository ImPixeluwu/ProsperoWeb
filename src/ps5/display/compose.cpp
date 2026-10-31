// SPDX-License-Identifier: GPL-3.0-or-later
#include "compose.h"

#include <algorithm>
#include <utility>

namespace ps5browser::display {

void copy_swap_row(const std::uint32_t* src, std::uint32_t* dst, int n) {
  for (int i = 0; i < n; i++) {
    dst[i] = swap_rb(src[i]);
  }
}

void blend_swap_row(const std::uint32_t* over, const std::uint32_t* under, std::uint32_t* dst,
                    int n) {
  for (int i = 0; i < n; i++) {
    dst[i] = swap_rb(blend_over(over[i], under[i]));
  }
}

void compose_rect(const Layers& layers, const std::vector<Rect>& chrome_rects, const Rect& r,
                  std::uint32_t* dst, int dst_stride) {
  const Rect area = intersect(r, Rect{0, 0, layers.width, layers.height});
  if (area.empty()) {
    return;
  }
  // The chrome rects that reach into the area, clipped to it.
  std::vector<Rect> chrome;
  for (const Rect& c : chrome_rects) {
    const Rect clipped = intersect(c, area);
    if (!clipped.empty()) {
      chrome.push_back(clipped);
    }
  }
  std::vector<std::pair<int, int>> spans;
  for (int y = area.y; y < area.bottom(); y++) {
    const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(layers.width);
    const std::uint32_t* page = layers.page + row;
    const std::uint32_t* over = layers.chrome + row;
    std::uint32_t* out = dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_stride);

    // Merge the chrome rects crossing this row into disjoint spans.
    spans.clear();
    for (const Rect& c : chrome) {
      if (y >= c.y && y < c.bottom()) {
        spans.emplace_back(c.x, c.right());
      }
    }
    std::sort(spans.begin(), spans.end());
    int x = area.x;
    for (std::size_t i = 0; i < spans.size();) {
      const int s0 = std::max(x, spans[i].first);
      int s1 = spans[i].second;
      for (++i; i < spans.size() && spans[i].first <= s1; ++i) {
        s1 = std::max(s1, spans[i].second);
      }
      if (s1 <= s0) {
        continue;
      }
      copy_swap_row(page + x, out + x, s0 - x);
      blend_swap_row(over + s0, page + s0, out + s0, s1 - s0);
      x = s1;
    }
    copy_swap_row(page + x, out + x, area.right() - x);
  }
}

}  // namespace ps5browser::display
