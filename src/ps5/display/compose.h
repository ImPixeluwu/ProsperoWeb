// SPDX-License-Identifier: GPL-3.0-or-later
//
// The UI compositor's pixel work: the page cache with the chrome cache
// blended over it inside the chrome rects, written into the window surface.
// Video overlay frames sit under the page: where the page punched a hole
// (alpha below 255) the video shows through, page pixels over it (controls,
// captions) blend on top.
//
// Formats: the caches hold what WKPagePaint writes, premultiplied ARGB32
// (0xAARRGGBB words); the PS5 SDL window surface is ABGR8888 (0xAABBGGRR
// words), the only format its driver offers. R and B are swapped on the way.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "damage_region.h"

namespace prosperobrowser::display {

// ARGB32 -> ABGR8888 (and back): swaps R and B.
inline std::uint32_t swap_rb(std::uint32_t p) {
  return (p & 0xFF00FF00u) | ((p & 0x00FF0000u) >> 16) | ((p & 0x000000FFu) << 16);
}

// Premultiplied source-over: src + dst * (255 - src.alpha) / 255 per channel,
// rounded, saturated. A fully transparent src leaves dst as is.
inline std::uint32_t blend_over(std::uint32_t src, std::uint32_t dst) {
  const std::uint32_t a = src >> 24;
  if (a == 0xFF) {
    return src;
  }
  if (a == 0) {
    return dst;
  }
  const std::uint32_t inv = 255 - a;
  std::uint32_t out = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    const std::uint32_t s = (src >> shift) & 0xFF;
    const std::uint32_t d = (dst >> shift) & 0xFF;
    const std::uint32_t t = d * inv + 128;
    const std::uint32_t scaled = (t + (t >> 8)) >> 8;  // d * inv / 255, rounded
    out |= std::min<std::uint32_t>(255, s + scaled) << shift;
  }
  return out;
}

// Row kernels: dst[i] = swap_rb(src[i]) and dst[i] = swap_rb(blend_over(over[i], under[i])).
// They use AVX2 when the CPU has it (always on the PS5's Zen 2) and match
// the scalar reference bit for bit.
void copy_swap_row(const std::uint32_t* src, std::uint32_t* dst, int n);
void blend_swap_row(const std::uint32_t* over, const std::uint32_t* under, std::uint32_t* dst,
                    int n);
// dst[i] = blend_over(over[i], under[i]) (no swap).
void blend_row(const std::uint32_t* over, const std::uint32_t* under, std::uint32_t* dst, int n);

// The variants behind the dispatch, for the tests. The _avx2 ones must only
// be called when has_avx2().
bool has_avx2();
void copy_swap_row_scalar(const std::uint32_t* src, std::uint32_t* dst, int n);
void blend_swap_row_scalar(const std::uint32_t* over, const std::uint32_t* under,
                           std::uint32_t* dst, int n);
void copy_swap_row_avx2(const std::uint32_t* src, std::uint32_t* dst, int n);
void blend_swap_row_avx2(const std::uint32_t* over, const std::uint32_t* under,
                         std::uint32_t* dst, int n);
void blend_row_scalar(const std::uint32_t* over, const std::uint32_t* under, std::uint32_t* dst,
                      int n);
void blend_row_avx2(const std::uint32_t* over, const std::uint32_t* under, std::uint32_t* dst,
                    int n);

// A video frame already scaled to its rect (window coordinates), shown only
// inside `clip` (the content area).
struct VideoLayer {
  const std::uint32_t* pixels = nullptr;
  int stride = 0;  // in pixels
  Rect rect;
  Rect clip;
};

// The two caches, both width x height with a stride of width pixels.
struct Layers {
  const std::uint32_t* page = nullptr;
  const std::uint32_t* chrome = nullptr;
  int width = 0;
  int height = 0;
};

// Composes rect r (clipped to the layers) into dst (stride in pixels): the
// videos (under the page, in order: a later one wins where they overlap),
// the page over them, and the chrome blended over that where a chrome rect
// covers it. Overlapping chrome rects blend each pixel once.
void compose_rect(const Layers& layers, const std::vector<Rect>& chrome_rects, const Rect& r,
                  std::uint32_t* dst, int dst_stride,
                  const std::vector<VideoLayer>& videos = {});

}  // namespace prosperobrowser::display
