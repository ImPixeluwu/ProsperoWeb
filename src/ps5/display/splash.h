// SPDX-License-Identifier: GPL-3.0-or-later
//
// The UI process's loading screen, drawn into the SDL window surface before
// WebKit paints anything: the chrome's dark background (chrome.css --bg),
// the product name in large light text, a dimmer "Loading…" line under it
// and a thin accent progress bar that advances with the startup stages.
//
// The text arrives as coverage images (rendered with FreeType by the
// frontend, ps5_splash_text.h); without them only the bar is drawn. Pixels
// are 32-bit words, ARGB32 (0xAARRGGBB) or, with swap_rb, ABGR8888 (the PS5
// window surface, see compose.h).
#pragma once

#include <cstdint>
#include <vector>

#include "damage_region.h"

namespace prosperobrowser::display {

// 8-bit coverage, row-major, width * height bytes.
struct GrayImage {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> coverage;

  bool empty() const { return width <= 0 || height <= 0; }
};

// chrome.css palette (ARGB32, opaque).
struct SplashColors {
  std::uint32_t background = 0xFF121418u;  // --bg
  std::uint32_t title = 0xFFEEF0F4u;       // --fg
  std::uint32_t subtitle = 0xFF9AA2B1u;    // --fg-dim
  std::uint32_t track = 0xFF262A32u;       // --bg-3
  std::uint32_t bar = 0xFF3D8BFFu;         // --accent
};

struct SplashLayout {
  Rect title;     // empty without a title image
  Rect subtitle;  // empty without a subtitle image
  Rect track;     // the whole progress bar
};

// Title and subtitle centred as one block slightly above the middle, the
// bar under them (centred alone when there is no text).
SplashLayout layout_splash(int width, int height, const GrayImage* title,
                           const GrayImage* subtitle);

struct SplashTarget {
  std::uint32_t* pixels = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;  // in pixels
  bool swap_rb = false;
};

// The whole screen. progress is clamped to 0..1.
void paint_splash(const SplashTarget& target, const SplashLayout& layout, const GrayImage* title,
                  const GrayImage* subtitle, double progress,
                  const SplashColors& colors = SplashColors{});

// Only the bar; returns the rect it repainted (the track, clipped).
Rect paint_splash_progress(const SplashTarget& target, const SplashLayout& layout,
                           double progress, const SplashColors& colors = SplashColors{});

}  // namespace prosperobrowser::display
