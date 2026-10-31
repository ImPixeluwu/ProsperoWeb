// SPDX-License-Identifier: GPL-3.0-or-later
//
// NV12 (8-bit 4:2:0: a Y plane, then interleaved Cb/Cr at half resolution)
// to the overlay's pixel format (ARGB32 words, alpha 0xFF), for pictures
// the UI decodes itself (hardware decoding, ps5_hwdec.cpp). The result goes
// through display::scale_image to the video box like a frame the WebProcess
// sent.
//
// Integer maths; full-size rows run an AVX2 loop (8 pixels at a time, the
// PS5 build compiles this file for Zen 2), the rest loops the compiler
// vectorises. Measured on a Skylake host: 1080p about 2 ms (scalar 7-8 ms),
// 4K halved to 1080p about 2.6 ms. `step` 2 halves both
// dimensions with a 2x2 box filter on luma (4K pictures shown at 1080p or
// below): a quarter of the conversion work and of the scaler's input.
#pragma once

#include <cstdint>
#include <vector>

namespace prosperoweb::video {

enum class YuvMatrix { Bt601, Bt709, Bt2020 };

struct YuvColor {
  YuvMatrix matrix = YuvMatrix::Bt709;
  bool full_range = false;
};

struct Nv12View {
  const std::uint8_t* y = nullptr;
  const std::uint8_t* uv = nullptr;
  int y_pitch = 0;   // bytes
  int uv_pitch = 0;  // bytes
  int width = 0;     // visible size (even sizes expected; odd ones are handled)
  int height = 0;
};

// Converts into dst ((width / step) x (height / step), dst_stride in
// pixels). step is 1 or 2. `scratch` is reused between calls.
void nv12_to_argb(const Nv12View& src, const YuvColor& color, int step, std::uint32_t* dst,
                  int dst_stride, std::vector<std::int32_t>& scratch);

// Tests: false forces the portable loops (the AVX2 row is used when the
// CPU has it; the PS5 build always does).
void set_nv12_simd(bool allowed);

// Floating-point reference of the same conversion (tests).
void nv12_to_argb_reference(const Nv12View& src, const YuvColor& color, int step,
                            std::uint32_t* dst, int dst_stride);

}  // namespace prosperoweb::video
