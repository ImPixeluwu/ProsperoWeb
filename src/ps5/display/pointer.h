// SPDX-License-Identifier: GPL-3.0-or-later
//
// The gamepad pointer ring, drawn by the UI compositor straight into the
// window surface on top of the page and the chrome. Moving it costs a
// recompose of two small rects, with no WebKit repaint and no IPC (the HTML
// chrome used to draw it, which repainted the chrome view on every move).
// It looks like the .pointer ring in chrome.css at 1920 px wide.
#pragma once

#include <cstdint>
#include <vector>

#include "damage_region.h"

namespace prosperobrowser::display {

// Premultiplied ARGB32 (0xAARRGGBB) square, centred on the hot spot.
struct PointerSprite {
  int size = 0;
  std::vector<std::uint32_t> pixels;
};

// Built once (antialiased, with a soft drop shadow).
const PointerSprite& pointer_sprite();

// The window rect the sprite covers when centred on (x, y).
Rect pointer_rect(int x, int y);

// Blends the sprite centred on (x, y) over dst (ABGR8888 words, the window
// surface), writing only inside clip.
void draw_pointer(std::uint32_t* dst, int dst_stride, const Rect& clip, int x, int y);

}  // namespace prosperobrowser::display
