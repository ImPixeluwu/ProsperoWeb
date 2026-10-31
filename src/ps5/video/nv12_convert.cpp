// SPDX-License-Identifier: GPL-3.0-or-later
#include "nv12_convert.h"

#include <algorithm>
#include <atomic>
#include <cmath>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace prosperoweb::video {

namespace {

struct Coefficients {
  double vr, ug, vg, ub;
};

// R = Y + vr Cr, G = Y - ug Cb - vg Cr, B = Y + ub Cb (full-range Y'CbCr).
Coefficients coefficients(YuvMatrix matrix) {
  switch (matrix) {
    case YuvMatrix::Bt601:
      return {1.402, 0.344136, 0.714136, 1.772};
    case YuvMatrix::Bt2020:
      return {1.4746, 0.164553, 0.571353, 1.8814};
    case YuvMatrix::Bt709:
      break;
  }
  return {1.5748, 0.187324, 0.468124, 1.8556};
}

constexpr int kShift = 14;
constexpr int kRound = 1 << (kShift - 1);

struct Fixed {
  int y_offset, y_scale, vr, ug, vg, ub;
};

Fixed fixed_coefficients(const YuvColor& color) {
  const Coefficients c = coefficients(color.matrix);
  const double ys = color.full_range ? 1.0 : 255.0 / 219.0;
  const double cs = color.full_range ? 1.0 : 255.0 / 224.0;
  auto fx = [](double v) { return static_cast<int>(std::lround(v * (1 << kShift))); };
  return {color.full_range ? 0 : 16, fx(ys), fx(c.vr * cs), fx(c.ug * cs), fx(c.vg * cs),
          fx(c.ub * cs)};
}

inline std::uint32_t clamp8(int v) { return static_cast<std::uint32_t>(std::clamp(v, 0, 255)); }

// One output row from luma samples `y` (already box-filtered for step 2)
// and per-pixel chroma terms.
void convert_row(const int* __restrict y, const int* __restrict rv, const int* __restrict gc,
                 const int* __restrict bu, int width, const Fixed& f,
                 std::uint32_t* __restrict out) {
  for (int x = 0; x < width; ++x) {
    const int yt = (y[x] - f.y_offset) * f.y_scale + kRound;
    const std::uint32_t r = clamp8((yt + rv[x]) >> kShift);
    const std::uint32_t g = clamp8((yt - gc[x]) >> kShift);
    const std::uint32_t b = clamp8((yt + bu[x]) >> kShift);
    out[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
  }
}

#if defined(__x86_64__)
std::atomic<bool> g_simd_allowed{true};

bool simd_available() {
#if defined(__AVX2__)
  return true;  // the PS5 build (Zen 2)
#else
  static const bool available = __builtin_cpu_supports("avx2");
  return available;
#endif
}

// A full-resolution row, 8 pixels per iteration: luma widened to 32 bits,
// the 4 chroma pairs of those pixels duplicated per pixel with a lane
// permute, the same integer maths as convert_row (bit-exact with it).
__attribute__((target("avx2"))) int convert_row_avx2(const std::uint8_t* y,
                                                      const std::uint8_t* uv, int width,
                                                      const Fixed& f, std::uint32_t* out) {
  const __m256i y_offset = _mm256_set1_epi32(f.y_offset);
  const __m256i y_scale = _mm256_set1_epi32(f.y_scale);
  const __m256i round = _mm256_set1_epi32(kRound);
  const __m256i c128 = _mm256_set1_epi32(128);
  const __m256i vr = _mm256_set1_epi32(f.vr);
  const __m256i ug = _mm256_set1_epi32(f.ug);
  const __m256i vg = _mm256_set1_epi32(f.vg);
  const __m256i ub = _mm256_set1_epi32(f.ub);
  const __m256i zero = _mm256_setzero_si256();
  const __m256i max = _mm256_set1_epi32(255);
  const __m256i alpha = _mm256_set1_epi32(static_cast<int>(0xFF000000u));
  const __m256i pick_u = _mm256_setr_epi32(0, 0, 2, 2, 4, 4, 6, 6);
  const __m256i pick_v = _mm256_setr_epi32(1, 1, 3, 3, 5, 5, 7, 7);
  int x = 0;
  for (; x + 8 <= width; x += 8) {
    const __m256i yy = _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(y + x)));
    const __m256i c = _mm256_sub_epi32(
        _mm256_cvtepu8_epi32(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(uv + x))), c128);
    const __m256i cb = _mm256_permutevar8x32_epi32(c, pick_u);
    const __m256i cr = _mm256_permutevar8x32_epi32(c, pick_v);
    const __m256i yt =
        _mm256_add_epi32(_mm256_mullo_epi32(_mm256_sub_epi32(yy, y_offset), y_scale), round);
    __m256i r = _mm256_srai_epi32(_mm256_add_epi32(yt, _mm256_mullo_epi32(cr, vr)), kShift);
    __m256i g = _mm256_srai_epi32(
        _mm256_sub_epi32(yt, _mm256_add_epi32(_mm256_mullo_epi32(cb, ug),
                                              _mm256_mullo_epi32(cr, vg))),
        kShift);
    __m256i b = _mm256_srai_epi32(_mm256_add_epi32(yt, _mm256_mullo_epi32(cb, ub)), kShift);
    r = _mm256_min_epi32(_mm256_max_epi32(r, zero), max);
    g = _mm256_min_epi32(_mm256_max_epi32(g, zero), max);
    b = _mm256_min_epi32(_mm256_max_epi32(b, zero), max);
    const __m256i pixel = _mm256_or_si256(
        _mm256_or_si256(alpha, _mm256_slli_epi32(r, 16)), _mm256_or_si256(_mm256_slli_epi32(g, 8), b));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + x), pixel);
  }
  return x;
}
#endif

}  // namespace

void set_nv12_simd(bool allowed) {
#if defined(__x86_64__)
  g_simd_allowed = allowed;
#else
  (void)allowed;
#endif
}

void nv12_to_argb(const Nv12View& src, const YuvColor& color, int step, std::uint32_t* dst,
                  int dst_stride, std::vector<std::int32_t>& scratch) {
  step = step == 2 ? 2 : 1;
  const int out_w = src.width / step;
  const int out_h = src.height / step;
  if (out_w <= 0 || out_h <= 0) {
    return;
  }
  const Fixed f = fixed_coefficients(color);
  const int chroma_w = (src.width + 1) / 2;
  scratch.resize(static_cast<std::size_t>(out_w) * 4);
  int* y = scratch.data();
  int* rv = y + out_w;
  int* gc = rv + out_w;
  int* bu = gc + out_w;
  for (int oy = 0; oy < out_h; ++oy) {
    // Chroma: one sample covers 2x2 luma, i.e. 2 output pixels at step 1
    // and exactly one at step 2.
    const int cy = step == 1 ? oy / 2 : oy;
    const std::uint8_t* uv = src.uv + static_cast<std::ptrdiff_t>(cy) * src.uv_pitch;
    if (step == 1) {
      int done = 0;
#if defined(__x86_64__)
      // Even widths only (the chroma pairs of the 8 pixels must exist).
      if (g_simd_allowed.load(std::memory_order_relaxed) && simd_available() && !(src.width & 1)) {
        done = convert_row_avx2(src.y + static_cast<std::ptrdiff_t>(oy) * src.y_pitch, uv, out_w,
                                f, dst + static_cast<std::ptrdiff_t>(oy) * dst_stride);
        if (done == out_w) {
          continue;
        }
      }
#endif
      for (int x = 0; x < out_w; ++x) {
        const int c = std::min(x >> 1, chroma_w - 1);
        const int cb = uv[2 * c] - 128;
        const int cr = uv[2 * c + 1] - 128;
        rv[x] = f.vr * cr;
        gc[x] = f.ug * cb + f.vg * cr;
        bu[x] = f.ub * cb;
      }
      const std::uint8_t* row = src.y + static_cast<std::ptrdiff_t>(oy) * src.y_pitch;
      for (int x = 0; x < out_w; ++x) {
        y[x] = row[x];
      }
    } else {
      for (int x = 0; x < out_w; ++x) {
        const int cb = uv[2 * x] - 128;
        const int cr = uv[2 * x + 1] - 128;
        rv[x] = f.vr * cr;
        gc[x] = f.ug * cb + f.vg * cr;
        bu[x] = f.ub * cb;
      }
      const std::uint8_t* r0 = src.y + static_cast<std::ptrdiff_t>(2 * oy) * src.y_pitch;
      const std::uint8_t* r1 = r0 + src.y_pitch;
      for (int x = 0; x < out_w; ++x) {
        y[x] = (r0[2 * x] + r0[2 * x + 1] + r1[2 * x] + r1[2 * x + 1] + 2) >> 2;
      }
    }
    convert_row(y, rv, gc, bu, out_w, f, dst + static_cast<std::ptrdiff_t>(oy) * dst_stride);
  }
}

void nv12_to_argb_reference(const Nv12View& src, const YuvColor& color, int step,
                            std::uint32_t* dst, int dst_stride) {
  step = step == 2 ? 2 : 1;
  const Coefficients c = coefficients(color.matrix);
  const double ys = color.full_range ? 1.0 : 255.0 / 219.0;
  const double cs = color.full_range ? 1.0 : 255.0 / 224.0;
  const double yo = color.full_range ? 0 : 16;
  const int out_w = src.width / step;
  const int out_h = src.height / step;
  for (int oy = 0; oy < out_h; ++oy) {
    for (int ox = 0; ox < out_w; ++ox) {
      double yv = 0;
      for (int dy = 0; dy < step; ++dy) {
        for (int dx = 0; dx < step; ++dx) {
          yv += src.y[(oy * step + dy) * src.y_pitch + ox * step + dx];
        }
      }
      yv /= step * step;
      const int cx = (ox * step) / 2;
      const int cyy = (oy * step) / 2;
      const double cb = (src.uv[cyy * src.uv_pitch + 2 * cx] - 128) * cs;
      const double cr = (src.uv[cyy * src.uv_pitch + 2 * cx + 1] - 128) * cs;
      const double yy = (yv - yo) * ys;
      auto q = [](double v) {
        return static_cast<std::uint32_t>(std::clamp(std::lround(v), 0L, 255L));
      };
      const std::uint32_t r = q(yy + c.vr * cr);
      const std::uint32_t g = q(yy - c.ug * cb - c.vg * cr);
      const std::uint32_t b = q(yy + c.ub * cb);
      dst[oy * dst_stride + ox] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
  }
}

}  // namespace prosperoweb::video
