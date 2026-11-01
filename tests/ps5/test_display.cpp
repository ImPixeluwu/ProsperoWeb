// SPDX-License-Identifier: GPL-3.0-or-later
//
// The UI compositor's pure parts (src/ps5/display): damage regions, the
// R/B-swapping copy and premultiplied blend kernels (AVX2 against the
// scalar reference), rect composition with chrome overlap, the frame
// statistics line and the loading screen.
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "compose.h"
#include "damage_region.h"
#include "frame_stats.h"
#include "pointer.h"
#include "splash.h"

using namespace prosperoweb::display;

namespace {

bool covered_by(const Region& region, const Rect& r) {
  for (int y = r.y; y < r.bottom(); y++) {
    for (int x = r.x; x < r.right(); x++) {
      if (!region.covers(x, y)) {
        return false;
      }
    }
  }
  return true;
}

std::uint32_t random_pixel(std::mt19937& rng) {
  // Bias towards the special alphas the kernels branch on.
  std::uint32_t p = rng();
  switch (rng() % 4) {
    case 0: return p & 0x00FFFFFFu;             // alpha 0 (colour left in: not premultiplied)
    case 1: return p | 0xFF000000u;             // opaque
    default: return p;                          // anything
  }
}

}  // namespace

TEST_SUITE("display_region") {
  TEST_CASE("rect intersect, bounding and containment") {
    const Rect a{0, 0, 10, 10};
    const Rect b{5, 5, 10, 10};
    CHECK(intersect(a, b) == Rect{5, 5, 5, 5});
    CHECK(intersect(a, Rect{20, 20, 5, 5}).empty());
    CHECK(intersect(a, Rect{10, 0, 5, 5}).empty());  // touching edges do not overlap
    CHECK(bounding(a, b) == Rect{0, 0, 15, 15});
    CHECK(bounding(Rect{}, b) == b);
    CHECK(a.contains(Rect{2, 2, 3, 3}));
    CHECK_FALSE(a.contains(b));
    CHECK(Rect{0, 0, 0, 5}.area() == 0);
    CHECK(Rect{0, 0, 1920, 1080}.area() == 1920u * 1080u);
  }

  TEST_CASE("empty and contained rects are not stored") {
    Region r;
    r.add(Rect{0, 0, 0, 10});
    CHECK(r.empty());
    r.add(Rect{0, 0, 100, 100});
    r.add(Rect{10, 10, 20, 20});
    CHECK(r.rects().size() == 1);
    r.add(Rect{-10, -10, 200, 200});  // swallows the first
    REQUIRE(r.rects().size() == 1);
    CHECK(r.rects()[0] == Rect{-10, -10, 200, 200});
  }

  TEST_CASE("adjacent halves merge, distant rects stay apart") {
    Region r;
    r.add(Rect{0, 0, 50, 100});
    r.add(Rect{50, 0, 50, 100});
    REQUIRE(r.rects().size() == 1);
    CHECK(r.rects()[0] == Rect{0, 0, 100, 100});

    Region apart;
    apart.add(Rect{0, 0, 200, 200});          // e.g. a video at the top left
    apart.add(Rect{1700, 1000, 200, 60});     // and a clock at the bottom right
    CHECK(apart.rects().size() == 2);
    CHECK(apart.area() == 200u * 200u + 200u * 60u);
    CHECK(apart.bounds() == Rect{0, 0, 1900, 1060});
  }

  TEST_CASE("the rect count is capped and coverage is kept") {
    std::mt19937 rng(7);
    Region r(4);
    std::vector<Rect> added;
    for (int i = 0; i < 40; i++) {
      const Rect rect{static_cast<int>(rng() % 1800), static_cast<int>(rng() % 1000),
                      1 + static_cast<int>(rng() % 60), 1 + static_cast<int>(rng() % 60)};
      added.push_back(rect);
      r.add(rect);
      CHECK(r.rects().size() <= 4);
    }
    bool all_covered = true;
    for (const Rect& rect : added) {
      all_covered = all_covered && covered_by(r, rect);
    }
    CHECK(all_covered);
  }

  TEST_CASE("clip and translate") {
    Region r;
    r.add(Rect{-50, -50, 100, 100});
    r.add(Rect{1900, 1000, 100, 100});
    r.clip(Rect{0, 0, 1920, 1080});
    REQUIRE(r.rects().size() == 2);
    CHECK(r.rects()[0] == Rect{0, 0, 50, 50});
    CHECK(r.rects()[1] == Rect{1900, 1000, 20, 80});
    r.translate(0, 96);
    CHECK(r.rects()[0] == Rect{0, 96, 50, 50});
    r.clip(Rect{3000, 3000, 10, 10});
    CHECK(r.empty());
  }
}

TEST_SUITE("display_pixels") {
  TEST_CASE("swap and blend reference") {
    CHECK(swap_rb(0xAA112233u) == 0xAA332211u);
    CHECK(swap_rb(swap_rb(0x12345678u)) == 0x12345678u);
    CHECK(blend_over(0x00000000u, 0xFF102030u) == 0xFF102030u);  // transparent
    CHECK(blend_over(0xFF405060u, 0xFF102030u) == 0xFF405060u);  // opaque
    // Half-transparent black over white: 255 * 127 / 255 = 127 per channel.
    CHECK(blend_over(0x80000000u, 0xFFFFFFFFu) == 0xFF7F7F7Fu);
    // Saturation: an over-bright (invalid) premultiplied source clips at 255.
    CHECK(blend_over(0x80FF0000u, 0xFFFF0000u) == 0xFFFF0000u);
  }

  TEST_CASE("dispatching kernels match the scalar reference") {
    std::mt19937 rng(42);
    for (int n : {0, 1, 7, 8, 9, 15, 16, 17, 33, 1920}) {
      std::vector<std::uint32_t> over(n), under(n), ref(n + 1, 0xDEADBEEFu), got(n + 1, 0xDEADBEEFu);
      for (int i = 0; i < n; i++) {
        over[i] = random_pixel(rng);
        under[i] = rng() | 0xFF000000u;
      }
      copy_swap_row_scalar(under.data(), ref.data(), n);
      copy_swap_row(under.data(), got.data(), n);
      CHECK_MESSAGE(ref == got, "copy n=", n);
      blend_swap_row_scalar(over.data(), under.data(), ref.data(), n);
      blend_swap_row(over.data(), under.data(), got.data(), n);
      CHECK_MESSAGE(ref == got, "blend n=", n);
      CHECK(got[n] == 0xDEADBEEFu);  // no write past n
    }
  }

  TEST_CASE("AVX2 kernels are bit-exact on every alpha") {
    if (!has_avx2()) {
      MESSAGE("no AVX2 on this host; the PS5 always has it");
      return;
    }
    // Every alpha with several colours, in vectors of 8 that mix alphas
    // and in uniform vectors that hit the fast paths.
    std::mt19937 rng(3);
    std::vector<std::uint32_t> over, under;
    for (std::uint32_t a = 0; a < 256; a++) {
      for (int k = 0; k < 8; k++) {
        const std::uint32_t c = rng() & 0x00FFFFFFu;
        over.push_back((a << 24) | c);
        under.push_back(rng());
      }
    }
    for (int k = 0; k < 4096; k++) {
      over.push_back(random_pixel(rng));
      under.push_back(rng());
    }
    const int n = static_cast<int>(over.size());
    std::vector<std::uint32_t> ref(n), got(n);
    blend_swap_row_scalar(over.data(), under.data(), ref.data(), n);
    blend_swap_row_avx2(over.data(), under.data(), got.data(), n);
    CHECK(ref == got);
    copy_swap_row_scalar(under.data(), ref.data(), n);
    copy_swap_row_avx2(under.data(), got.data(), n);
    CHECK(ref == got);
  }

  TEST_CASE("compose_rect composes only the rect, chrome blended once") {
    const int w = 64;
    const int h = 40;
    std::mt19937 rng(11);
    std::vector<std::uint32_t> page(w * h), chrome(w * h);
    for (auto& p : page) {
      p = rng() | 0xFF000000u;
    }
    for (auto& p : chrome) {
      p = random_pixel(rng);
    }
    const std::vector<Rect> chrome_rects{{0, 0, 64, 8}, {10, 4, 20, 20}, {15, 10, 5, 5},
                                         {60, 30, 30, 30}};
    const int stride = w + 5;  // the surface pitch may exceed the width
    std::vector<std::uint32_t> out(stride * h, 0x12345678u);
    const Rect area{5, 2, 50, 30};
    compose_rect(Layers{page.data(), chrome.data(), w, h}, chrome_rects, area, out.data(), stride);

    int wrong = 0;
    int untouched_outside = 0;
    int outside = 0;
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        const std::uint32_t got = out[y * stride + x];
        const bool inside = x >= area.x && x < area.right() && y >= area.y && y < area.bottom();
        if (!inside) {
          outside++;
          untouched_outside += got == 0x12345678u;
          continue;
        }
        bool in_chrome = false;
        for (const Rect& c : chrome_rects) {
          in_chrome = in_chrome || (x >= c.x && x < c.right() && y >= c.y && y < c.bottom());
        }
        const std::uint32_t p = page[y * w + x];
        const std::uint32_t want =
            swap_rb(in_chrome ? blend_over(chrome[y * w + x], p) : p);
        wrong += got != want;
      }
    }
    CHECK(wrong == 0);
    CHECK(untouched_outside == outside);
  }

  TEST_CASE("compose_rect clips to the layers") {
    std::vector<std::uint32_t> page(16, 0xFF0000FFu), chrome(16, 0u), out(16, 0u);
    compose_rect(Layers{page.data(), chrome.data(), 4, 4}, {}, Rect{2, 2, 100, 100}, out.data(), 4);
    CHECK(out[2 * 4 + 2] == 0xFFFF0000u);
    CHECK(out[3 * 4 + 3] == 0xFFFF0000u);
    CHECK(out[1 * 4 + 1] == 0u);
    compose_rect(Layers{page.data(), chrome.data(), 4, 4}, {}, Rect{-8, -8, 4, 4}, out.data(), 4);
    CHECK(out[0] == 0u);
  }
}

TEST_SUITE("display_stats") {
  TEST_CASE("timing and area stats") {
    TimingStat t;
    CHECK(t.avg_ms() == 0.0);
    t.add(2.0, 1000);
    t.add(4.0, 3000);
    CHECK(t.count == 2);
    CHECK(t.avg_ms() == doctest::Approx(3.0));
    CHECK(t.max_ms == doctest::Approx(4.0));
    CHECK(t.avg_kpx() == doctest::Approx(2.0));
    TimingStat u;
    u.add(9.0, 0);
    t.merge(u);
    CHECK(t.count == 3);
    CHECK(t.max_ms == doctest::Approx(9.0));
  }

  TEST_CASE("the log line") {
    FrameStats s;
    CHECK_FALSE(s.active());
    s.frames = 300;
    CHECK_FALSE(s.active());  // an idle UI is not logged
    s.page_needs_display.add(640 * 360);
    s.present.add(8.0, 640 * 360);
    s.damage_rects = 1;
    CHECK(s.active());
    const std::string line = s.format(5.0);
    CHECK(line.find("perf 5.0s: ui 60.0 fps") == 0);
    CHECK(line.find("ndisp page 0.2/s 230kpx") != std::string::npos);
    CHECK(line.find("present 0.2/s 8.00/8.00ms 1.0 rects 230kpx") != std::string::npos);
  }
}

// --- Loading screen (splash.h) ---------------------------------------------

namespace {

GrayImage solid_image(int w, int h, std::uint8_t a) {
  GrayImage img;
  img.width = w;
  img.height = h;
  img.coverage.assign(static_cast<std::size_t>(w) * h, a);
  return img;
}

}  // namespace

TEST_SUITE("display_splash") {
  TEST_CASE("layout centres the text block and puts the bar under it") {
    const GrayImage title = solid_image(600, 130, 255);
    const GrayImage subtitle = solid_image(120, 40, 255);
    const SplashLayout l = layout_splash(1920, 1080, &title, &subtitle);
    CHECK(l.title == Rect{660, l.title.y, 600, 130});
    CHECK(l.subtitle.x == 900);
    CHECK(l.subtitle.y > l.title.bottom());
    CHECK(l.track.y > l.subtitle.bottom());
    CHECK(l.track.w == 480);
    CHECK(l.track.x == 720);
    // Above the middle of the screen.
    CHECK(l.title.y + (l.track.bottom() - l.title.y) / 2 < 540);
    // No text: only the bar, no text rects.
    const SplashLayout bare = layout_splash(1920, 1080, nullptr, nullptr);
    CHECK(bare.title.empty());
    CHECK(bare.subtitle.empty());
    CHECK(bare.track.w == 480);
  }

  TEST_CASE("paints background, text and progress in the surface format") {
    const int w = 640, h = 360;
    std::vector<std::uint32_t> px(static_cast<std::size_t>(w) * h, 0xDEADBEEFu);
    const GrayImage title = solid_image(100, 20, 255);
    GrayImage subtitle = solid_image(40, 10, 0);
    subtitle.coverage[0] = 128;  // one half-covered pixel
    const SplashLayout l = layout_splash(w, h, &title, &subtitle);
    const SplashColors c;
    paint_splash(SplashTarget{px.data(), w, h, w, false}, l, &title, &subtitle, 0.5);
    auto at = [&](int x, int y) { return px[static_cast<std::size_t>(y) * w + x]; };
    CHECK(at(0, 0) == c.background);
    CHECK(at(w - 1, h - 1) == c.background);
    CHECK(at(l.title.x + 5, l.title.y + 5) == c.title);
    // Half coverage: halfway between --bg and --fg-dim.
    const std::uint32_t half = at(l.subtitle.x, l.subtitle.y);
    CHECK(((half >> 16) & 0xFF) == (0x12 + 0x9A + 1) / 2);
    CHECK(at(l.subtitle.x + 1, l.subtitle.y) == c.background);
    // Half the bar done (track is 480 wide but clipped to 640: 480).
    CHECK(at(l.track.x, l.track.y) == c.bar);
    CHECK(at(l.track.x + l.track.w / 2 - 1, l.track.y) == c.bar);
    CHECK(at(l.track.x + l.track.w / 2, l.track.y) == c.track);
    CHECK(at(l.track.right() - 1, l.track.bottom() - 1) == c.track);

    // ABGR8888 surface: R and B swapped.
    paint_splash(SplashTarget{px.data(), w, h, w, true}, l, nullptr, nullptr, 1.0);
    CHECK(at(0, 0) == swap_rb(c.background));
    CHECK(at(l.track.right() - 1, l.track.y) == swap_rb(c.bar));
    CHECK(at(l.title.x + 5, l.title.y + 5) == swap_rb(c.background));  // no text this time
  }

  TEST_CASE("message fills the background and centres the lines, no bar") {
    const int w = 640, h = 360;
    std::vector<std::uint32_t> px(static_cast<std::size_t>(w) * h, 0xDEADBEEFu);
    const GrayImage l1 = solid_image(300, 30, 255);
    const GrayImage l2 = solid_image(400, 20, 255);
    const SplashColors c;
    paint_message(SplashTarget{px.data(), w, h, w, false}, {&l1, &l2}, 16);
    auto at = [&](int x, int y) { return px[static_cast<std::size_t>(y) * w + x]; };
    // Background everywhere it is not text.
    CHECK(at(0, 0) == c.background);
    CHECK(at(w - 1, h - 1) == c.background);
    // The block is centred horizontally and a little above the middle.
    const int block_h = 30 + 16 + 20;
    int y = (h - block_h) / 2 - h / 20;
    CHECK(at((w - 300) / 2 + 2, y + 2) == c.title);          // first line: title colour
    CHECK(at((w - 400) / 2 + 2, y + 30 + 16 + 2) == c.subtitle);  // rest: dimmer
    // Null and empty lines are skipped without drawing.
    std::fill(px.begin(), px.end(), 0u);
    const GrayImage empty;
    paint_message(SplashTarget{px.data(), w, h, w, false}, {nullptr, &empty});
    CHECK(px[0] == c.background);
  }

  TEST_CASE("progress repaints only the bar, clamped") {
    const int w = 640, h = 360;
    std::vector<std::uint32_t> px(static_cast<std::size_t>(w) * h, 0u);
    const SplashLayout l = layout_splash(w, h, nullptr, nullptr);
    const SplashColors c;
    const SplashTarget t{px.data(), w, h, w, false};
    const Rect r = paint_splash_progress(t, l, 2.0);
    CHECK(r == l.track);
    CHECK(px[static_cast<std::size_t>(l.track.y) * w + l.track.right() - 1] == c.bar);
    CHECK(px[0] == 0u);  // untouched outside the bar
    paint_splash_progress(t, l, -1.0);
    CHECK(px[static_cast<std::size_t>(l.track.y) * w + l.track.x] == c.track);
    // A target smaller than the bar: clipped, no write outside.
    std::vector<std::uint32_t> tiny(100 * 50, 0u);
    const SplashLayout tl = layout_splash(100, 50, nullptr, nullptr);
    const Rect tr = paint_splash_progress(SplashTarget{tiny.data(), 100, 50, 100, false}, tl, 1.0);
    CHECK(tr.w == 100);
  }
}

TEST_SUITE("display_pointer") {
  using namespace prosperoweb::display;

  TEST_CASE("the sprite is a white ring with a white dot over a translucent fill") {
    const PointerSprite& s = pointer_sprite();
    REQUIRE(s.size > 0);
    REQUIRE(s.pixels.size() == static_cast<std::size_t>(s.size) * s.size);
    const int c = s.size / 2;
    auto at = [&](int x, int y) { return s.pixels[static_cast<std::size_t>(y) * s.size + x]; };
    CHECK(at(c, c) == 0xFFFFFFFFu);                 // dot
    CHECK(at(c + 16, c) == 0xFFFFFFFFu);            // border (radius 15..18)
    const std::uint32_t fill = at(c + 9, c);        // fill: translucent blue
    CHECK((fill >> 24) > 0u);
    CHECK((fill >> 24) < 0x80u);
    CHECK((fill & 0xFFu) > ((fill >> 16) & 0xFFu));  // more blue than red
    CHECK(at(0, 0) == 0u);                          // corner: transparent
  }

  TEST_CASE("pointer_rect is centred on the position") {
    const Rect r = pointer_rect(100, 50);
    CHECK(r.x + r.w / 2 == 100);
    CHECK(r.y + r.h / 2 == 50);
  }

  TEST_CASE("draw_pointer blends over ABGR pixels and stays inside the clip") {
    constexpr int kW = 200;
    constexpr int kH = 100;
    std::vector<std::uint32_t> surface(kW * kH, 0xFF0000FFu);  // ABGR: opaque red
    const Rect clip{0, 0, 100, kH};
    draw_pointer(surface.data(), kW, clip, 98, 50);
    CHECK(surface[50 * kW + 98] == 0xFFFFFFFFu);    // dot drawn
    CHECK(surface[50 * kW + 101] == 0xFF0000FFu);   // right of the clip: untouched
    CHECK(surface[0] == 0xFF0000FFu);               // far away: untouched
    // Inside the fill the red shows through, tinted.
    const std::uint32_t tinted = surface[50 * kW + 89];
    CHECK((tinted & 0xFFu) > 0x80u);                // red (low byte in ABGR) stays strong
    CHECK(((tinted >> 16) & 0xFFu) > 0u);           // some blue added
    // Off-surface positions only touch what the clip allows.
    draw_pointer(surface.data(), kW, Rect{0, 0, kW, kH}, -40, -40);
    CHECK(surface[0] == 0xFF0000FFu);
  }
}
