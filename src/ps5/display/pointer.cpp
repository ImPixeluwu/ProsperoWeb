// SPDX-License-Identifier: GPL-3.0-or-later
#include "pointer.h"

#include <algorithm>
#include <cmath>

#include "compose.h"

namespace prosperobrowser::display {

namespace {

// chrome.css .pointer at 16 px per rem: a 36 px ring with a 3 px white
// border, a translucent blue fill, a 2 px dark outline, a 6 px white dot
// and a shadow 4 px below.
constexpr int kSize = 64;
constexpr float kCentre = kSize / 2.0f;
constexpr float kRadius = 18.0f;
constexpr float kBorder = 3.0f;
constexpr float kOutline = 2.0f;
constexpr float kDot = 3.0f;
constexpr float kShadowOffset = 4.0f;
constexpr float kShadowBlur = 6.0f;

struct Rgba {
  float r = 0, g = 0, b = 0, a = 0;  // premultiplied
};

Rgba over(Rgba src, Rgba dst) {
  const float k = 1.0f - src.a;
  return {src.r + dst.r * k, src.g + dst.g * k, src.b + dst.b * k, src.a + dst.a * k};
}

Rgba colour(float r, float g, float b, float a, float coverage) {
  const float alpha = a * std::clamp(coverage, 0.0f, 1.0f);
  return {r * alpha, g * alpha, b * alpha, alpha};
}

// Antialiased coverage of the annulus inner <= d < outer at distance d.
float ring(float d, float inner, float outer) {
  return std::clamp(outer - d + 0.5f, 0.0f, 1.0f) * std::clamp(d - inner + 0.5f, 0.0f, 1.0f);
}

float disc(float d, float radius) { return std::clamp(radius - d + 0.5f, 0.0f, 1.0f); }

std::uint32_t pack(Rgba c) {
  auto byte = [](float v) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
  };
  return byte(c.a) << 24 | byte(c.r) << 16 | byte(c.g) << 8 | byte(c.b);
}

PointerSprite build() {
  PointerSprite s;
  s.size = kSize;
  s.pixels.resize(static_cast<std::size_t>(kSize) * kSize);
  const float outer = kRadius + kOutline;
  for (int y = 0; y < kSize; y++) {
    for (int x = 0; x < kSize; x++) {
      const float px = static_cast<float>(x) + 0.5f - kCentre;
      const float py = static_cast<float>(y) + 0.5f - kCentre;
      const float d = std::hypot(px, py);
      const float ds = std::hypot(px, py - kShadowOffset);
      // Shadow: fades out over kShadowBlur on both sides of the ring edge.
      const float shadow_t =
          std::clamp((ds - (outer - kShadowBlur)) / (2.0f * kShadowBlur), 0.0f, 1.0f);
      const float shadow = 1.0f - shadow_t * shadow_t * (3.0f - 2.0f * shadow_t);
      // Like CSS outer shadows it is not painted under the ring itself.
      const float outside = std::clamp(d - kRadius + 0.5f, 0.0f, 1.0f);
      Rgba c = colour(0, 0, 0, 0.45f, shadow * outside);
      c = over(colour(0, 0, 0, 0.55f, ring(d, kRadius, outer)), c);
      c = over(colour(61 / 255.0f, 139 / 255.0f, 1.0f, 0.25f, disc(d, kRadius - kBorder)), c);
      c = over(colour(1, 1, 1, 1, ring(d, kRadius - kBorder, kRadius)), c);
      c = over(colour(1, 1, 1, 1, disc(d, kDot)), c);
      s.pixels[static_cast<std::size_t>(y) * kSize + x] = pack(c);
    }
  }
  return s;
}

}  // namespace

const PointerSprite& pointer_sprite() {
  static const PointerSprite sprite = build();
  return sprite;
}

Rect pointer_rect(int x, int y) {
  const int half = kSize / 2;
  return Rect{x - half, y - half, kSize, kSize};
}

void draw_pointer(std::uint32_t* dst, int dst_stride, const Rect& clip, int x, int y) {
  const PointerSprite& s = pointer_sprite();
  const Rect r = pointer_rect(x, y);
  const Rect area = intersect(r, clip);
  for (int row = area.y; row < area.bottom(); row++) {
    const std::uint32_t* src = s.pixels.data() + static_cast<std::size_t>(row - r.y) * s.size;
    std::uint32_t* out = dst + static_cast<std::size_t>(row) * static_cast<std::size_t>(dst_stride);
    for (int col = area.x; col < area.right(); col++) {
      // The surface is ABGR8888: blend in ARGB and swap back.
      out[col] = swap_rb(blend_over(src[col - r.x], swap_rb(out[col])));
    }
  }
}

}  // namespace prosperobrowser::display
