// SPDX-License-Identifier: GPL-3.0-or-later
//
// Image scaling for the UI's video overlay: an opaque video frame (BGRA
// bytes, i.e. ARGB32 words like the page cache) resized to the element's
// rect on screen. The output keeps the cache format (the compositor swaps
// R/B on the way to the window surface) with alpha forced to 0xFF.
//
// Sampling: pixel centres, dst (x + 0.5) maps to src (x + 0.5) * sw / dw,
// edges clamped (the usual bilinear convention, as swscale/Skia).
#pragma once

#include <cstdint>
#include <vector>

namespace ps5browser::display {

enum class ScaleFilter { Nearest, Bilinear };

struct ImageView {
  const std::uint32_t* pixels = nullptr;
  int width = 0;
  int height = 0;
  int stride = 0;  // in pixels
};

// Scales src into dst (dst_w x dst_h, dst_stride in pixels). Same size is a
// copy. Bilinear uses 8-bit weights (within 1-2 of the exact result per
// channel); `scratch` is reused between calls to avoid allocations.
void scale_image(const ImageView& src, std::uint32_t* dst, int dst_w, int dst_h, int dst_stride,
                 ScaleFilter filter, std::vector<std::uint32_t>& scratch);

// The exact (floating-point, rounded) bilinear result, for the tests.
void scale_image_reference(const ImageView& src, std::uint32_t* dst, int dst_w, int dst_h,
                           int dst_stride);

}  // namespace ps5browser::display
