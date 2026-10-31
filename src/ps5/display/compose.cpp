// SPDX-License-Identifier: GPL-3.0-or-later
#include "compose.h"

#include <algorithm>
#include <utility>

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace prosperobrowser::display {

void copy_swap_row_scalar(const std::uint32_t* src, std::uint32_t* dst, int n) {
  for (int i = 0; i < n; i++) {
    dst[i] = swap_rb(src[i]);
  }
}

void blend_swap_row_scalar(const std::uint32_t* over, const std::uint32_t* under,
                           std::uint32_t* dst, int n) {
  for (int i = 0; i < n; i++) {
    dst[i] = swap_rb(blend_over(over[i], under[i]));
  }
}

void blend_row_scalar(const std::uint32_t* over, const std::uint32_t* under, std::uint32_t* dst,
                      int n) {
  for (int i = 0; i < n; i++) {
    dst[i] = blend_over(over[i], under[i]);
  }
}

#if defined(__x86_64__)

bool has_avx2() {
#if defined(__PROSPERO__)
  return true;  // Zen 2; SDL's PS5 tile blit relies on it as well
#else
  static const bool avx2 = __builtin_cpu_supports("avx2");
  return avx2;
#endif
}

namespace {

// Byte shuffle swapping bytes 0 and 2 of every pixel (B <-> R in memory).
__attribute__((target("avx2"))) inline __m256i swap_rb_mask() {
  return _mm256_setr_epi8(2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15,  //
                          2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15);
}

// d * inv / 255 rounded, per 16-bit lane (inputs <= 255), as blend_over().
__attribute__((target("avx2"))) inline __m256i scale_div255(__m256i d, __m256i inv) {
  const __m256i t = _mm256_add_epi16(_mm256_mullo_epi16(d, inv), _mm256_set1_epi16(128));
  return _mm256_srli_epi16(_mm256_add_epi16(t, _mm256_srli_epi16(t, 8)), 8);
}

// 255 - alpha, broadcast to the four channel lanes of each unpacked pixel.
__attribute__((target("avx2"))) inline __m256i inverse_alpha(__m256i s16) {
  const __m256i a = _mm256_shufflehi_epi16(_mm256_shufflelo_epi16(s16, 0xFF), 0xFF);
  return _mm256_sub_epi16(_mm256_set1_epi16(255), a);
}

}  // namespace

__attribute__((target("avx2"))) void copy_swap_row_avx2(const std::uint32_t* src,
                                                        std::uint32_t* dst, int n) {
  const __m256i swap = swap_rb_mask();
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_shuffle_epi8(v, swap));
  }
  copy_swap_row_scalar(src + i, dst + i, n - i);
}

// The blend of blend_swap_row_avx2() and blend_row_avx2(): Swap selects
// whether the result is R/B-swapped on the way out.
template <bool Swap>
__attribute__((target("avx2"))) inline void blend_row_avx2_impl(const std::uint32_t* over,
                                                                const std::uint32_t* under,
                                                                std::uint32_t* dst, int n) {
  const __m256i swap = swap_rb_mask();
  const __m256i zero = _mm256_setzero_si256();
  const __m256i alpha_mask = _mm256_set1_epi32(static_cast<int>(0xFF000000u));
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    const __m256i s = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(over + i));
    const __m256i d = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(under + i));
    const __m256i a = _mm256_and_si256(s, alpha_mask);
    const __m256i transparent = _mm256_cmpeq_epi32(a, zero);
    __m256i out;
    if (_mm256_movemask_epi8(transparent) == -1) {
      out = d;  // the common case: no chrome pixel here
    } else if (_mm256_movemask_epi8(_mm256_cmpeq_epi32(a, alpha_mask)) == -1) {
      out = s;  // opaque chrome
    } else {
      const __m256i s_lo = _mm256_unpacklo_epi8(s, zero);
      const __m256i s_hi = _mm256_unpackhi_epi8(s, zero);
      const __m256i d_lo = scale_div255(_mm256_unpacklo_epi8(d, zero), inverse_alpha(s_lo));
      const __m256i d_hi = scale_div255(_mm256_unpackhi_epi8(d, zero), inverse_alpha(s_hi));
      // s + d * (255 - a) / 255, saturated; alpha 255 gives s (the scaled
      // term is 0), alpha 0 must give d exactly (also for non-zero colour).
      out = _mm256_adds_epu8(s, _mm256_packus_epi16(d_lo, d_hi));
      out = _mm256_blendv_epi8(out, d, transparent);
    }
    if constexpr (Swap) {
      out = _mm256_shuffle_epi8(out, swap);
    }
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), out);
  }
  if constexpr (Swap) {
    blend_swap_row_scalar(over + i, under + i, dst + i, n - i);
  } else {
    blend_row_scalar(over + i, under + i, dst + i, n - i);
  }
}

__attribute__((target("avx2"))) void blend_swap_row_avx2(const std::uint32_t* over,
                                                         const std::uint32_t* under,
                                                         std::uint32_t* dst, int n) {
  blend_row_avx2_impl<true>(over, under, dst, n);
}

__attribute__((target("avx2"))) void blend_row_avx2(const std::uint32_t* over,
                                                    const std::uint32_t* under,
                                                    std::uint32_t* dst, int n) {
  blend_row_avx2_impl<false>(over, under, dst, n);
}

#else  // !__x86_64__

bool has_avx2() { return false; }
void copy_swap_row_avx2(const std::uint32_t* src, std::uint32_t* dst, int n) {
  copy_swap_row_scalar(src, dst, n);
}
void blend_swap_row_avx2(const std::uint32_t* over, const std::uint32_t* under,
                         std::uint32_t* dst, int n) {
  blend_swap_row_scalar(over, under, dst, n);
}
void blend_row_avx2(const std::uint32_t* over, const std::uint32_t* under, std::uint32_t* dst,
                    int n) {
  blend_row_scalar(over, under, dst, n);
}

#endif

void copy_swap_row(const std::uint32_t* src, std::uint32_t* dst, int n) {
  if (n <= 0) {
    return;
  }
  if (has_avx2()) {
    copy_swap_row_avx2(src, dst, n);
  } else {
    copy_swap_row_scalar(src, dst, n);
  }
}

void blend_swap_row(const std::uint32_t* over, const std::uint32_t* under, std::uint32_t* dst,
                    int n) {
  if (n <= 0) {
    return;
  }
  if (has_avx2()) {
    blend_swap_row_avx2(over, under, dst, n);
  } else {
    blend_swap_row_scalar(over, under, dst, n);
  }
}

void blend_row(const std::uint32_t* over, const std::uint32_t* under, std::uint32_t* dst, int n) {
  if (n <= 0) {
    return;
  }
  if (has_avx2()) {
    blend_row_avx2(over, under, dst, n);
  } else {
    blend_row_scalar(over, under, dst, n);
  }
}

void compose_rect(const Layers& layers, const std::vector<Rect>& chrome_rects, const Rect& r,
                  std::uint32_t* dst, int dst_stride, const std::vector<VideoLayer>& video_layers) {
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
  // The video layers that reach into the area (visible part only).
  struct Video {
    const VideoLayer* layer;
    Rect visible;
  };
  std::vector<Video> videos;
  for (const VideoLayer& v : video_layers) {
    const Rect visible = intersect(intersect(v.rect, v.clip), area);
    if (v.pixels && !visible.empty()) {
      videos.push_back({&v, visible});
    }
  }
  // Page over video, per row, where a video is under the page.
  std::vector<std::uint32_t> base;
  if (!videos.empty()) {
    base.resize(static_cast<std::size_t>(layers.width));
  }
  std::vector<std::pair<int, int>> spans;
  for (int y = area.y; y < area.bottom(); y++) {
    const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(layers.width);
    const std::uint32_t* page = layers.page + row;
    bool row_has_video = false;
    for (const Video& v : videos) {
      if (y < v.visible.y || y >= v.visible.bottom()) {
        continue;
      }
      if (!row_has_video) {
        std::copy(page + area.x, page + area.right(), base.begin() + area.x);
        row_has_video = true;
      }
      const VideoLayer& layer = *v.layer;
      const std::uint32_t* under =
          layer.pixels + static_cast<std::size_t>(y - layer.rect.y) * static_cast<std::size_t>(layer.stride) +
          static_cast<std::size_t>(v.visible.x - layer.rect.x);
      // The page as it was (not a previous video's result) goes on top.
      blend_row(page + v.visible.x, under, base.data() + v.visible.x, v.visible.w);
    }
    if (row_has_video) {
      page = base.data();
    }
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

}  // namespace prosperobrowser::display
