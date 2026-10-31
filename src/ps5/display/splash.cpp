// SPDX-License-Identifier: GPL-3.0-or-later
#include "splash.h"

#include <algorithm>
#include <cmath>

#include "compose.h"

namespace prosperoweb::display {

namespace {

constexpr int kTitleGap = 28;    // title -> subtitle
constexpr int kBarGap = 48;      // text -> bar
constexpr int kBarWidth = 480;
constexpr int kBarHeight = 4;

std::uint32_t mix(std::uint32_t bg, std::uint32_t fg, std::uint32_t a) {
  if (a == 0) {
    return bg;
  }
  if (a == 255) {
    return fg;
  }
  std::uint32_t out = 0xFF000000u;
  for (int shift = 0; shift < 24; shift += 8) {
    const std::uint32_t b = (bg >> shift) & 0xFF;
    const std::uint32_t f = (fg >> shift) & 0xFF;
    const std::uint32_t t = f * a + b * (255 - a) + 128;
    out |= ((t + (t >> 8)) >> 8) << shift;
  }
  return out;
}

std::uint32_t out_pixel(const SplashTarget& t, std::uint32_t argb) {
  return t.swap_rb ? swap_rb(argb) : argb;
}

void fill(const SplashTarget& t, const Rect& r, std::uint32_t argb) {
  const Rect c = intersect(r, Rect{0, 0, t.width, t.height});
  const std::uint32_t p = out_pixel(t, argb);
  for (int y = c.y; y < c.bottom(); y++) {
    std::uint32_t* row = t.pixels + static_cast<std::size_t>(y) * t.stride;
    std::fill(row + c.x, row + c.right(), p);
  }
}

void draw_text(const SplashTarget& t, const Rect& at, const GrayImage& image,
               std::uint32_t background, std::uint32_t colour) {
  for (int y = 0; y < image.height; y++) {
    const int ty = at.y + y;
    if (ty < 0 || ty >= t.height) {
      continue;
    }
    std::uint32_t* row = t.pixels + static_cast<std::size_t>(ty) * t.stride;
    for (int x = 0; x < image.width; x++) {
      const int tx = at.x + x;
      if (tx < 0 || tx >= t.width) {
        continue;
      }
      const std::uint8_t a = image.coverage[static_cast<std::size_t>(y) * image.width + x];
      if (a) {
        row[tx] = out_pixel(t, mix(background, colour, a));
      }
    }
  }
}

}  // namespace

SplashLayout layout_splash(int width, int height, const GrayImage* title,
                           const GrayImage* subtitle) {
  const bool has_title = title && !title->empty();
  const bool has_subtitle = has_title && subtitle && !subtitle->empty();
  const int text_h = (has_title ? title->height : 0) +
                     (has_subtitle ? kTitleGap + subtitle->height : 0);
  const int block_h = text_h + (has_title ? kBarGap : 0) + kBarHeight;
  // A little above the centre, where the eye expects a title.
  int y = (height - block_h) / 2 - height / 20;
  y = std::max(0, y);
  SplashLayout out;
  if (has_title) {
    out.title = Rect{(width - title->width) / 2, y, title->width, title->height};
    y += title->height;
    if (has_subtitle) {
      y += kTitleGap;
      out.subtitle = Rect{(width - subtitle->width) / 2, y, subtitle->width, subtitle->height};
      y += subtitle->height;
    }
    y += kBarGap;
  }
  const int bar_w = std::min(kBarWidth, width);
  out.track = Rect{(width - bar_w) / 2, y, bar_w, kBarHeight};
  return out;
}

void paint_splash(const SplashTarget& target, const SplashLayout& layout, const GrayImage* title,
                  const GrayImage* subtitle, double progress, const SplashColors& colors) {
  fill(target, Rect{0, 0, target.width, target.height}, colors.background);
  if (title && !title->empty() && !layout.title.empty()) {
    draw_text(target, layout.title, *title, colors.background, colors.title);
  }
  if (subtitle && !subtitle->empty() && !layout.subtitle.empty()) {
    draw_text(target, layout.subtitle, *subtitle, colors.background, colors.subtitle);
  }
  paint_splash_progress(target, layout, progress, colors);
}

Rect paint_splash_progress(const SplashTarget& target, const SplashLayout& layout,
                           double progress, const SplashColors& colors) {
  const double p = std::clamp(std::isnan(progress) ? 0.0 : progress, 0.0, 1.0);
  const Rect track = layout.track;
  const int done = static_cast<int>(std::lround(track.w * p));
  fill(target, track, colors.track);
  fill(target, Rect{track.x, track.y, done, track.h}, colors.bar);
  return intersect(track, Rect{0, 0, target.width, target.height});
}

}  // namespace prosperoweb::display
