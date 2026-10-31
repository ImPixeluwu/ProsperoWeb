// SPDX-License-Identifier: GPL-3.0-or-later
#include "scale.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace prosperoweb::display {

namespace {

constexpr std::uint32_t kOpaque = 0xFF000000u;

// Source coordinate of destination sample i: index of the left/top tap and
// the weight of the right/bottom one (0..256), clamped at the edges.
struct Tap {
  int i0;
  int i1;
  std::uint32_t w;
};

std::vector<Tap> make_taps(int src, int dst) {
  std::vector<Tap> taps(static_cast<std::size_t>(dst));
  const double scale = static_cast<double>(src) / dst;
  for (int i = 0; i < dst; i++) {
    double s = (i + 0.5) * scale - 0.5;
    if (s < 0) {
      s = 0;
    }
    int i0 = static_cast<int>(s);
    if (i0 >= src - 1) {
      taps[static_cast<std::size_t>(i)] = Tap{src - 1, src - 1, 0};
      continue;
    }
    auto w = static_cast<std::uint32_t>(std::lround((s - i0) * 256.0));
    if (w >= 256) {
      // Rounded up onto the next sample.
      taps[static_cast<std::size_t>(i)] = Tap{i0 + 1, i0 + 1, 0};
      continue;
    }
    taps[static_cast<std::size_t>(i)] = Tap{i0, i0 + 1, w};
  }
  return taps;
}

// a * (256 - w) + b * w, divided by 256, two 8-bit channels per 32-bit lane.
inline std::uint32_t lerp(std::uint32_t a, std::uint32_t b, std::uint32_t w) {
  const std::uint32_t iw = 256 - w;
  const std::uint32_t rb = (((a & 0x00FF00FFu) * iw + (b & 0x00FF00FFu) * w) >> 8) & 0x00FF00FFu;
  const std::uint32_t ag = ((((a >> 8) & 0x00FF00FFu) * iw + ((b >> 8) & 0x00FF00FFu) * w)) &
                           0xFF00FF00u;
  return rb | ag;
}

void horizontal(const std::uint32_t* row, const std::vector<Tap>& taps, std::uint32_t* out) {
  const std::size_t n = taps.size();
  for (std::size_t i = 0; i < n; i++) {
    const Tap& t = taps[i];
    out[i] = t.w ? lerp(row[t.i0], row[t.i1], t.w) : row[t.i0];
  }
}

}  // namespace

void scale_image(const ImageView& src, std::uint32_t* dst, int dst_w, int dst_h, int dst_stride,
                 ScaleFilter filter, std::vector<std::uint32_t>& scratch) {
  if (!src.pixels || src.width <= 0 || src.height <= 0 || dst_w <= 0 || dst_h <= 0) {
    return;
  }
  auto src_row = [&](int y) {
    return src.pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(src.stride);
  };
  auto dst_row = [&](int y) {
    return dst + static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_stride);
  };
  if (src.width == dst_w && src.height == dst_h) {
    for (int y = 0; y < dst_h; y++) {
      const std::uint32_t* in = src_row(y);
      std::uint32_t* out = dst_row(y);
      for (int x = 0; x < dst_w; x++) {
        out[x] = in[x] | kOpaque;
      }
    }
    return;
  }
  if (filter == ScaleFilter::Nearest) {
    std::vector<int> xs(static_cast<std::size_t>(dst_w));
    for (int x = 0; x < dst_w; x++) {
      xs[static_cast<std::size_t>(x)] =
          std::min(src.width - 1, static_cast<int>((x + 0.5) * src.width / dst_w));
    }
    for (int y = 0; y < dst_h; y++) {
      const int sy = std::min(src.height - 1, static_cast<int>((y + 0.5) * src.height / dst_h));
      const std::uint32_t* in = src_row(sy);
      std::uint32_t* out = dst_row(y);
      for (int x = 0; x < dst_w; x++) {
        out[x] = in[xs[static_cast<std::size_t>(x)]] | kOpaque;
      }
    }
    return;
  }

  // Separable bilinear: each source row needed is interpolated horizontally
  // once (two cached rows), then the two rows are blended vertically.
  const std::vector<Tap> xt = make_taps(src.width, dst_w);
  const std::vector<Tap> yt = make_taps(src.height, dst_h);
  const auto w = static_cast<std::size_t>(dst_w);
  scratch.resize(2 * w);
  std::uint32_t* rows[2] = {scratch.data(), scratch.data() + w};
  int cached[2] = {-1, -1};
  // Makes rows[0] hold source row i0 and rows[1] row i1 (horizontally
  // interpolated); consecutive output rows mostly reuse them.
  auto ensure = [&](int i0, int i1) {
    if (cached[0] != i0) {
      if (cached[1] == i0) {
        std::swap(rows[0], rows[1]);
        std::swap(cached[0], cached[1]);
      } else {
        horizontal(src_row(i0), xt, rows[0]);
        cached[0] = i0;
      }
    }
    if (i1 != i0 && cached[1] != i1) {
      horizontal(src_row(i1), xt, rows[1]);
      cached[1] = i1;
    }
  };
  for (int y = 0; y < dst_h; y++) {
    const Tap& t = yt[static_cast<std::size_t>(y)];
    std::uint32_t* out = dst_row(y);
    ensure(t.i0, t.w ? t.i1 : t.i0);
    const std::uint32_t* a = rows[0];
    if (!t.w) {
      for (std::size_t x = 0; x < w; x++) {
        out[x] = a[x] | kOpaque;
      }
      continue;
    }
    const std::uint32_t* b = rows[1];
    for (std::size_t x = 0; x < w; x++) {
      out[x] = lerp(a[x], b[x], t.w) | kOpaque;
    }
  }
}

void scale_image_reference(const ImageView& src, std::uint32_t* dst, int dst_w, int dst_h,
                           int dst_stride) {
  auto sample = [&](int x, int y, int shift) {
    return static_cast<double>(
        (src.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(src.stride) +
                    static_cast<std::size_t>(x)] >>
         shift) &
        0xFF);
  };
  for (int y = 0; y < dst_h; y++) {
    double sy = std::clamp((y + 0.5) * src.height / dst_h - 0.5, 0.0, src.height - 1.0);
    const int y0 = static_cast<int>(sy);
    const int y1 = std::min(y0 + 1, src.height - 1);
    const double fy = sy - y0;
    for (int x = 0; x < dst_w; x++) {
      double sx = std::clamp((x + 0.5) * src.width / dst_w - 0.5, 0.0, src.width - 1.0);
      const int x0 = static_cast<int>(sx);
      const int x1 = std::min(x0 + 1, src.width - 1);
      const double fx = sx - x0;
      std::uint32_t out = kOpaque;
      for (int shift = 0; shift < 24; shift += 8) {
        const double top = sample(x0, y0, shift) * (1 - fx) + sample(x1, y0, shift) * fx;
        const double bottom = sample(x0, y1, shift) * (1 - fx) + sample(x1, y1, shift) * fx;
        const double v = top * (1 - fy) + bottom * fy;
        out |= static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0, 255.0))) << shift;
      }
      dst[static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_stride) +
          static_cast<std::size_t>(x)] = out;
    }
  }
}

}  // namespace prosperoweb::display
